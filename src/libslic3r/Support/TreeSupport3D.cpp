// Tree supports by Thomas Rahm, losely based on Tree Supports by CuraEngine.
// Original source of Thomas Rahm's tree supports:
// https://github.com/ThomasRahm/CuraEngine
//
// Original CuraEngine copyright:
// Copyright (c) 2021 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#include "TreeSupport3D.hpp"
#include "AABBTreeIndirect.hpp"
#include "AABBTreeLines.hpp"
#include "BuildVolume.hpp"
#include "BridgeDetector.hpp"
#include "ClipperUtils.hpp"
#include "EdgeGrid.hpp"
#include "Fill/Fill.hpp"
#include "Layer.hpp"
#include "Print.hpp"
#include "MultiPoint.hpp"
#include "Polygon.hpp"
#include "Polyline.hpp"
#include "MutablePolygon.hpp"
#include "BeltFloorContext.hpp"
#include "libslic3r/Support/TreeSupportCommon.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Support/TreeModelVolumes.hpp"
#include "libslic3r/Support/SupportParameters.hpp"
#include "libslic3r/Support/SupportLayer.hpp"
#include "SupportCommon.hpp"
#include "SupportCacheBudget.hpp"
#include "SupportPolygonAccumulator.hpp"
#include "OrganicSupportLayerPlan.hpp"
#include "OrganicSupportPathBearing.hpp"
#include "TriangleMeshSlicer.hpp"
#include "TreeSupport.hpp"
#include "I18N.hpp"

#include <cassert>
#include <chrono>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iomanip>
#include <thread>
#include <optional>
#include <new>
#include <stdio.h>
#include <string>
#include <string_view>

#include <boost/log/trivial.hpp>

#include <tbb/parallel_for.h>
#include <tbb/parallel_for_each.h>
#include <tbb/spin_mutex.h>
#include <tbb/task_arena.h>
#include <vector>
#include <utility>
#include <unordered_set>
#include <unordered_map>
#include <tuple>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/MultiMaterialSegmentation.hpp"
#include "libslic3r/Slicing.hpp"

#if defined(TREE_SUPPORT_SHOW_ERRORS) && defined(_WIN32)
    #define TREE_SUPPORT_SHOW_ERRORS_WIN32
#endif

#define TREE_SUPPORT_ORGANIC_NUDGE_NEW 1

#ifndef TREE_SUPPORT_ORGANIC_NUDGE_NEW
// Old version using OpenVDB, works but it is extremely slow for complex meshes.
#include "../OpenVDBUtilsLegacy.hpp"
#include <openvdb/tools/VolumeToSpheres.h>
#endif // TREE_SUPPORT_ORGANIC_NUDGE_NEW

namespace Slic3r {
namespace TreeSupport3D {

static void restore_generator_layers_to_topology(
    SupportGeneratorLayersPtr &layers,
    const SlicingParameters &slicing_params,
    const TreeSupportSettings &config)
{
    for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx)
        if (SupportGeneratorLayer *layer = layers[layer_idx]; layer != nullptr)
            layer_initialize(*layer, slicing_params, config, layer_idx);
}

static bool remap_generator_layers(
    SupportGeneratorLayersPtr &layers,
    const OrganicSupportLayerPlan &plan,
    bool require_exact_z)
{
    if (layers.empty())
        return true;
    SupportGeneratorLayersPtr source_layers = layers;
    if (require_exact_z) {
        for (const SupportGeneratorLayer *layer : source_layers)
            if (layer != nullptr && !layer->polygons.empty() && plan.find_exact(layer->print_z) < 0)
                return false;
    }
    SupportGeneratorLayersPtr remapped(plan.size(), nullptr);
    for (SupportGeneratorLayer *layer : source_layers) {
        if (layer == nullptr || layer->polygons.empty())
            continue;
        const LayerIndex output_idx = require_exact_z ? plan.find_exact(layer->print_z) : plan.at_or_below(layer->print_z);
        if (output_idx < 0 || output_idx >= LayerIndex(plan.size()))
            return false;
        plan.initialize(*layer, size_t(output_idx));
        SupportGeneratorLayer *&destination = remapped[size_t(output_idx)];
        if (destination == nullptr)
            destination = layer;
        else if (destination != layer)
            destination->merge(std::move(*layer));
    }
    layers = std::move(remapped);
    return true;
}

static void normalize_organic_bottom_contacts(
    const PrintObject &object, TreeModelVolumes &volumes, const TreeSupportSettings &config,
    const SupportParameters &support_params,
    const OrganicSupportLayerPlan &plan,
    SupportGeneratorLayersPtr &bottom_contacts,
    SupportGeneratorLayersPtr &intermediate_layers,
    SupportGeneratorLayerStorage &storage);

static const Layer *organic_bottom_contact_model_layer(
    const PrintObject &object, const Polygons &footprint, coordf_t expected_model_z,
    const std::vector<BoundingBox> *model_layer_bounds = nullptr)
{
    if (footprint.empty())
        return nullptr;
    const BoundingBox footprint_bounds = get_extents(footprint);
    if (!footprint_bounds.defined)
        return nullptr;
    const Layer *model_layer = nullptr;
    double best_overlap = 0.;
    const double min_area = sqr(double(scale_(0.001)));
    const double footprint_area = area(footprint);
    const auto &layers = object.layers();
    const auto end = std::upper_bound(layers.begin(), layers.end(), expected_model_z + EPSILON,
        [](coordf_t z, const Layer *layer) { return z < layer->print_z; });
    // Search from the expected surface downward.  A complete overlap is the
    // maximum possible score, and the historical tie rule prefers the higher
    // layer, so it is safe to stop immediately once it is found.  Partial
    // overlaps still run the same exact scan and selection rule below.
    for (auto it = end; it != layers.begin();) {
        --it;
        const Layer *candidate = *it;
        if (model_layer_bounds != nullptr) {
            const size_t index = size_t(std::distance(layers.begin(), it));
            if (index >= model_layer_bounds->size() ||
                !(*model_layer_bounds)[index].defined ||
                !footprint_bounds.overlap((*model_layer_bounds)[index]))
                continue;
        }
        const double overlap = area(intersection(candidate->lslices, footprint));
        // A grazing side wall must not replace the platform carrying the root.
        if (overlap > best_overlap + min_area ||
            (overlap > min_area && std::abs(overlap - best_overlap) <= min_area &&
             (model_layer == nullptr || candidate->print_z > model_layer->print_z))) {
            model_layer = candidate;
            best_overlap = overlap;
        }
        if (model_layer == candidate && footprint_area > min_area &&
            overlap + min_area >= footprint_area)
            return candidate;
    }
    return model_layer;
}

// smooth_outward() only changes duplicate/near-duplicate vertices and
// concave corners.  Convex polygons without such vertices are already in the
// fixed point of that operation, so avoid walking every vertex again while
// preserving the exact native routine for every polygon that may change.
static bool organic_polygon_needs_smoothing(const Polygon &polygon)
{
    if (polygon.size() < 3)
        return true;
    const coord_t duplicate_limit = scale_(0.01);
    const int64_t duplicate_limit2 = sqr<int64_t>(duplicate_limit);
    for (size_t idx = 0; idx < polygon.size(); ++idx) {
        const Point &p0 = polygon[(idx + polygon.size() - 1) % polygon.size()];
        const Point &p1 = polygon[idx];
        const Point &p2 = polygon[(idx + 1) % polygon.size()];
        if ((p1 - p0).cast<int64_t>().squaredNorm() <= duplicate_limit2 ||
            cross2((p0 - p1).cast<int64_t>(), (p2 - p1).cast<int64_t>()) > 0)
            return true;
    }
    return false;
}

static Polygons organic_smooth_outward_if_needed(Polygons polygons, coord_t distance)
{
    for (Polygon &polygon : polygons) {
        if (!organic_polygon_needs_smoothing(polygon))
            continue;
        Polygons single;
        single.emplace_back(std::move(polygon));
        single = smooth_outward(std::move(single), distance);
        polygon = single.empty() ? Polygon{} : std::move(single.front());
    }
    polygons.erase(std::remove_if(polygons.begin(), polygons.end(),
        [](const Polygon &polygon) { return polygon.empty(); }), polygons.end());
    return polygons;
}

// Opt-in geometry evidence from the actual generator, before G-code export.
static void trace_organic_stage(const char *stage, const PrintObject &object,
    std::initializer_list<std::pair<const char *, const SupportGeneratorLayersPtr *>> groups)
{
    const char *path = std::getenv("INLONG_ORGANIC_STAGE_TRACE");
    if (path == nullptr)
        return;
    std::ofstream out(path, std::ios::app);
    out << std::setprecision(15);
    auto polygons = [&out](const Polygons &areas) {
        out << '[';
        bool first = true;
        for (const Polygon &polygon : areas) {
            if (!first) out << ',';
            first = false;
            out << '[';
            for (size_t i = 0; i < polygon.points.size(); ++i) {
                if (i) out << ',';
                out << '[' << unscale<double>(polygon.points[i].x()) << ',' << unscale<double>(polygon.points[i].y()) << ']';
            }
            out << ']';
        }
        out << ']';
    };
    out << "{\"stage\":\"" << stage << "\",\"groups\":{";
    bool first_group = true;
    for (const auto &[name, layers] : groups) {
        if (!first_group) out << ',';
        first_group = false;
        out << '"' << name << "\":[";
        bool first_layer = true;
        for (const SupportGeneratorLayer *layer : *layers) {
            if (layer == nullptr || layer->polygons.empty()) continue;
            if (!first_layer) out << ',';
            first_layer = false;
            out << "{\"z\":" << layer->print_z << ",\"bottom\":" << layer->bottom_z
                << ",\"height\":" << layer->height << ",\"type\":" << int(layer->layer_type)
                << ",\"model_above\":" << layer->idx_object_layer_above << ",\"polygons\":";
            polygons(layer->polygons);
            if (layer->layer_type == SupporLayerType::BottomContact && layer->contact_polygons) {
                out << ",\"wall_polygons\":";
                polygons(*layer->contact_polygons);
            }
            out << '}';
        }
        out << ']';
    }
    out << "},\"model\":[";
    bool first = true;
    for (const Layer *layer : object.layers()) {
        if (!first) out << ',';
        first = false;
        out << "{\"z\":" << layer->print_z << ",\"height\":" << layer->height << ",\"polygons\":";
        polygons(to_polygons(layer->lslices));
        out << '}';
    }
    out << "]}\n";
}

// Optional stage-only trace for diagnosing a long organic-support generation.
// Unlike trace_organic_stage(), this writes no geometry and is inactive unless
// explicitly requested by the caller.
static void trace_organic_timing(const char *stage)
{
    const char *path = std::getenv("INLONG_ORGANIC_TIMING_TRACE");
    if (path == nullptr || *path == '\0' || stage == nullptr)
        return;
    static std::mutex trace_mutex;
    static const auto trace_start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - trace_start).count();
    std::lock_guard<std::mutex> lock(trace_mutex);
    std::ofstream out(path, std::ios::app);
    if (out)
        out << std::fixed << std::setprecision(3) << elapsed << " " << stage << "\n";
}

static void trace_organic_timing_progress(const char *stage, size_t done, size_t total)
{
    const char *path = std::getenv("INLONG_ORGANIC_TIMING_TRACE");
    if (path == nullptr || *path == '\0' || stage == nullptr)
        return;
    static std::mutex trace_mutex;
    static const auto trace_start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - trace_start).count();
    std::lock_guard<std::mutex> lock(trace_mutex);
    std::ofstream out(path, std::ios::app);
    if (out)
        out << std::fixed << std::setprecision(3) << elapsed << " " << stage << " " << done << "/" << total << "\n";
}

} // namespace TreeSupport3D
} // namespace Slic3r

#ifndef _L
#define _L(s) Slic3r::I18N::translate(s)
#endif


namespace Slic3r
{

namespace TreeSupport3D
{

using LineInformation = std::vector<std::pair<Point, LineStatus>>;
using LineInformations = std::vector<LineInformation>;
using namespace std::literals;

static inline void validate_range(const Point &pt)
{
    static constexpr const int32_t hi = 65536 * 16384;
    if (pt.x() > hi || pt.y() > hi || -pt.x() > hi || -pt.y() > hi)
      throw RuntimeError("Coordinate outside allowed range");
}

static inline void validate_range(const Points &points)
{
    for (const Point &p : points)
        validate_range(p);
}

static inline void validate_range(const MultiPoint &mp)
{
    validate_range(mp.points);
}

static inline void validate_range(const Polygons &polygons)
{
    for (const Polygon &p : polygons)
        validate_range(p);
}

static inline void validate_range(const Polylines &polylines)
{
    for (const Polyline &p : polylines)
        validate_range(p);
}

static inline void validate_range(const LineInformation &lines)
{
    for (const auto& p : lines)
        validate_range(p.first);
}

static inline void validate_range(const LineInformations &lines)
{
    for (const LineInformation &l : lines)
        validate_range(l);
}

static inline void check_self_intersections(const Polygons &polygons, const std::string_view message)
{
#ifdef TREE_SUPPORT_SHOW_ERRORS_WIN32
    if (!intersecting_edges(polygons).empty())
        ::MessageBoxA(nullptr, (std::string("TreeSupport infill self intersections: ") + std::string(message)).c_str(), "Bug detected!", MB_OK | MB_SYSTEMMODAL | MB_SETFOREGROUND | MB_ICONWARNING);
#endif // TREE_SUPPORT_SHOW_ERRORS_WIN32
}
static inline void check_self_intersections(const ExPolygon &expoly, const std::string_view message)
{
#ifdef TREE_SUPPORT_SHOW_ERRORS_WIN32
    check_self_intersections(to_polygons(expoly), message);
#endif // TREE_SUPPORT_SHOW_ERRORS_WIN32
}

static std::vector<std::pair<TreeSupportSettings, std::vector<size_t>>> group_meshes(const Print &print, const std::vector<size_t> &print_object_ids)
{
    std::vector<std::pair<TreeSupportSettings, std::vector<size_t>>> grouped_meshes;

    // Inlong: Recompute static mesh-group state for this support generation pass.
    TreeSupportSettings::zero_top_z_gap = false;

    //FIXME this is ugly, it does not belong here.
    for (size_t object_id : print_object_ids) {
        const PrintObject       &print_object  = *print.get_object(object_id);
        const PrintObjectConfig &object_config = print_object.config();
        if (object_config.support_top_z_distance < EPSILON)
            // || min_feature_size < scaled<coord_t>(0.1) that is the minimum line width
            TreeSupportSettings::zero_top_z_gap = true;
    }

    size_t largest_printed_mesh_idx = 0;

    // Group all meshes that can be processed together. NOTE this is different from mesh-groups! Only one setting object is needed per group,
    // as different settings in the same group may only occur in the tip, which uses the original settings objects from the meshes.
    for (size_t object_id : print_object_ids) {
        const PrintObject       &print_object  = *print.get_object(object_id);

        bool found_existing_group = false;
        TreeSupportSettings next_settings{ TreeSupportMeshGroupSettings{ print_object }, print_object.slicing_parameters() };
        //FIXME for now only a single object per group is enabled.
#if 0
        for (size_t idx = 0; idx < grouped_meshes.size(); ++ idx)
            if (next_settings == grouped_meshes[idx].first) {
                found_existing_group = true;
                grouped_meshes[idx].second.emplace_back(object_id);
                // handle some settings that are only used for performance reasons. This ensures that a horrible set setting intended to improve performance can not reduce it drastically.
                grouped_meshes[idx].first.performance_interface_skip_layers = std::min(grouped_meshes[idx].first.performance_interface_skip_layers, next_settings.performance_interface_skip_layers);
            }
#endif
        if (! found_existing_group)
            grouped_meshes.emplace_back(next_settings, std::vector<size_t>{ object_id });

        // no need to do this per mesh group as adaptive layers and raft setting are not setable per mesh.
        if (print.get_object(largest_printed_mesh_idx)->layers().back()->print_z < print_object.layers().back()->print_z)
            largest_printed_mesh_idx = object_id;
    }

#if 0
    {
        std::vector<coord_t> known_z(storage.meshes[largest_printed_mesh_idx].layers.size());
        for (size_t z = 0; z < storage.meshes[largest_printed_mesh_idx].layers.size(); z++)
            known_z[z] = storage.meshes[largest_printed_mesh_idx].layers[z].printZ;
        for (size_t idx = 0; idx < grouped_meshes.size(); ++ idx)
            grouped_meshes[idx].first.setActualZ(known_z);
    }
#endif

    return grouped_meshes;
}

#if 0
// todo remove as only for debugging relevant
[[nodiscard]] static std::string getPolygonAsString(const Polygons& poly)
{
    std::string ret;
    for (auto path : poly)
        for (Point p : path) {
            if (ret != "")
                ret += ",";
            ret += "(" + std::to_string(p.x()) + "," + std::to_string(p.y()) + ")";
        }
    return ret;
}

[[nodiscard]] static const std::vector<Polygons> generate_overhangs(const TreeSupportSettings &settings, PrintObject &print_object, std::function<void()> throw_on_cancel)
{
    const size_t num_raft_layers   = settings.raft_layers.size();
    const size_t num_object_layers = print_object.layer_count();
    const size_t num_layers        = num_object_layers + num_raft_layers;
    std::vector<Polygons> out(num_layers, Polygons{});

    const PrintConfig       &print_config           = print_object.print()->config();
    const PrintObjectConfig &config                 = print_object.config();
    const bool               support_auto           = config.enable_support.value && is_auto(config.support_type.value);
    const int                support_enforce_layers = config.enforce_support_layers.value;
    std::vector<Polygons>    enforcers_layers{ print_object.slice_support_enforcers() };
    std::vector<Polygons>    blockers_layers{ print_object.slice_support_blockers() };
    print_object.project_and_append_custom_facets(false, EnforcerBlockerType::ENFORCER, enforcers_layers);
    print_object.project_and_append_custom_facets(false, EnforcerBlockerType::BLOCKER, blockers_layers);
    const int                support_threshold      = config.support_threshold_angle.value;
    const bool               support_threshold_auto = support_threshold == 0;
    // +1 makes the threshold inclusive
    double                   tan_threshold          = support_threshold_auto ? 0. : tan(M_PI * double(support_threshold + 1) / 180.);
    // Build plate tilt: compute per-layer XY shift for tilted gravity direction
    const Vec2d              tilt_slope             = build_plate_tilt_slope(print_config);
    const bool               has_tilt               = tilt_slope.cwiseAbs().maxCoeff() > EPSILON;
    //FIXME this is a fudge constant!
    auto                     enforcer_overhang_offset = scaled<double>(config.tree_support_tip_diameter.value);
    const coordf_t radius_sample_resolution = g_config_tree_support_collision_resolution;

    // calc the extrudable expolygons of each layer
    const coordf_t extrusion_width = config.line_width.value;
    const coordf_t extrusion_width_scaled = scale_(extrusion_width);
    tbb::parallel_for(tbb::blocked_range<size_t>(0, print_object.layer_count()),
        [&](const tbb::blocked_range<size_t>& range) {
            for (size_t layer_nr = range.begin(); layer_nr < range.end(); layer_nr++) {
                if (print_object.print()->canceled())
                    break;
                Layer* layer = print_object.get_layer(layer_nr);
                // Filter out areas whose diameter that is smaller than extrusion_width, but we don't want to lose any details.
                layer->lslices_extrudable = intersection_ex(layer->lslices, offset2_ex(layer->lslices, -extrusion_width_scaled / 2, extrusion_width_scaled));
            }
        });

    size_t num_overhang_layers = support_auto ? num_object_layers : std::min(num_object_layers, std::max(size_t(support_enforce_layers), enforcers_layers.size()));
    tbb::parallel_for(tbb::blocked_range<LayerIndex>(1, num_overhang_layers),
        [&print_object, &config, &print_config, &enforcers_layers, &blockers_layers,
         support_auto, support_enforce_layers, support_threshold_auto, tan_threshold, enforcer_overhang_offset, num_raft_layers, radius_sample_resolution, has_tilt, tilt_slope, &throw_on_cancel, &out]
        (const tbb::blocked_range<LayerIndex> &range) {
        for (LayerIndex layer_id = range.begin(); layer_id < range.end(); ++ layer_id) {
            const Layer   &current_layer  = *print_object.get_layer(layer_id);
            const Layer   &lower_layer    = *print_object.get_layer(layer_id - 1);
            // Full overhangs with zero lower_layer_offset and no blockers applied.
            Polygons       raw_overhangs;
            bool           raw_overhangs_calculated = false;
            // Final overhangs.
            Polygons       overhangs;
            // For how many layers full overhangs shall be supported.
            const bool     enforced_layer = layer_id < support_enforce_layers;
            if (support_auto || enforced_layer) {
                float lower_layer_offset;
                if (enforced_layer)
                    lower_layer_offset = 0;
                else if (support_threshold_auto) {
                    float external_perimeter_width = 0;
                    for (const LayerRegion *layerm : lower_layer.regions())
                        external_perimeter_width += layerm->flow(frExternalPerimeter).scaled_width();
                    external_perimeter_width /= lower_layer.region_count();
                    lower_layer_offset = external_perimeter_width - float(scale_(config.support_threshold_overlap.get_abs_value(unscale_(external_perimeter_width))));
                } else
                    lower_layer_offset = scaled<float>(lower_layer.height / tan_threshold);
                // Apply build plate tilt: shift lower layer polygons to simulate tilted gravity
                Polygons lower_layer_offseted;
                if (has_tilt) {
                    Polygons lower_src = to_polygons(lower_layer.lslices_extrudable);
                    translate(lower_src, Point::new_scale(tilt_slope * lower_layer.height));
                    lower_layer_offseted = offset(lower_src, lower_layer_offset);
                } else {
                    lower_layer_offseted = offset(lower_layer.lslices_extrudable, lower_layer_offset);
                }
                overhangs = diff(current_layer.lslices_extrudable, lower_layer_offseted);
                if (lower_layer_offset == 0) {
                    raw_overhangs = overhangs;
                    raw_overhangs_calculated = true;
                }
                if (! (enforced_layer || blockers_layers.empty() || blockers_layers[layer_id].empty()))
                    overhangs = diff(overhangs, offset_ex(union_(blockers_layers[layer_id]), scale_(radius_sample_resolution)), ApplySafetyOffset::Yes);
                //if (config.bridge_no_support) {
                //    for (const LayerRegion *layerm : current_layer.regions())
                //        remove_bridges_from_contacts(print_config, lower_layer, *layerm,
                //            float(layerm->flow(frExternalPerimeter).scaled_width()), overhangs);
                //}
            }
            //check_self_intersections(overhangs, "generate_overhangs1");
            if (! enforcers_layers.empty() && ! enforcers_layers[layer_id].empty()) {
                // Has some support enforcers at this layer, apply them to the overhangs, don't apply the support threshold angle.
                //enforcers_layers[layer_id] = union_(enforcers_layers[layer_id]);
                //check_self_intersections(enforcers_layers[layer_id], "generate_overhangs - enforcers");
                //check_self_intersections(to_polygons(lower_layer.lslices), "generate_overhangs - lowerlayers");
                if (Polygons enforced_overhangs = intersection(raw_overhangs_calculated ? raw_overhangs : diff(current_layer.lslices, lower_layer.lslices), enforcers_layers[layer_id] /*, ApplySafetyOffset::Yes */);
                    ! enforced_overhangs.empty()) {
                    //FIXME this is a hack to make enforcers work on steep overhangs.
                    //check_self_intersections(enforced_overhangs, "generate_overhangs - enforced overhangs1");
                    //Polygons enforced_overhangs_prev = enforced_overhangs;
                    //check_self_intersections(to_polygons(union_ex(enforced_overhangs)), "generate_overhangs - enforced overhangs11");
                    //check_self_intersections(offset(union_ex(enforced_overhangs),
                    //FIXME enforcer_overhang_offset is a fudge constant!
                    enforced_overhangs = diff(offset(union_ex(enforced_overhangs), enforcer_overhang_offset),
                        lower_layer.lslices);
                    //check_self_intersections(enforced_overhangs, "generate_overhangs - enforced overhangs2");
                    overhangs = overhangs.empty() ? std::move(enforced_overhangs) : union_(overhangs, enforced_overhangs);
                    //check_self_intersections(overhangs, "generate_overhangs - enforcers");
                }
            }
            out[layer_id + num_raft_layers] = std::move(overhangs);
            throw_on_cancel();
        }
    });

#if 0
    if (num_raft_layers > 0) {
        const Layer   &first_layer = *print_object.get_layer(0);
        // Final overhangs.
        Polygons       overhangs = 
            // Don't apply blockes on raft layer.
            //(! blockers_layers.empty() && ! blockers_layers[layer_id].empty() ? 
            //    diff(first_layer.lslices, blockers_layers[layer_id], ApplySafetyOffset::Yes) :
                to_polygons(first_layer.lslices);
#if 0
        if (! enforcers_layers.empty() && ! enforcers_layers[layer_id].empty()) {
            if (Polygons enforced_overhangs = intersection(first_layer.lslices, enforcers_layers[layer_id] /*, ApplySafetyOffset::Yes */);
                ! enforced_overhangs.empty()) {
                //FIXME this is a hack to make enforcers work on steep overhangs.
                //FIXME enforcer_overhang_offset is a fudge constant!
                enforced_overhangs = offset(union_ex(enforced_overhangs), enforcer_overhang_offset);
                overhangs = overhangs.empty() ? std::move(enforced_overhangs) : union_(overhangs, enforced_overhangs);
            }
        }   
#endif
        out[num_raft_layers] = std::move(overhangs);
        throw_on_cancel();
    }
#endif

    return out;
}
#endif

/*!
 * \brief Precalculates all avoidances, that could be required.
 *
 * \param storage[in] Background storage to access meshes.
 * \param currently_processing_meshes[in] Indexes of all meshes that are processed in this iteration
 */
[[nodiscard]] static LayerIndex precalculate(const Print &print, const std::vector<Polygons> &overhangs, const TreeSupportSettings &config, const std::vector<size_t> &object_ids, TreeModelVolumes &volumes, std::function<void()> throw_on_cancel)
{
    // calculate top most layer that is relevant for support
    LayerIndex max_layer = 0;
    for (size_t object_id : object_ids) {
        const PrintObject &print_object      = *print.get_object(object_id);
        const int       num_raft_layers      = int(config.raft_layers.size());
        const int       num_layers           = int(print_object.layer_count()) + num_raft_layers;
        int             max_support_layer_id = 0;
        for (int layer_id = std::max<int>(num_raft_layers, 1); layer_id < num_layers; ++ layer_id)
            if (! overhangs[layer_id].empty())
                max_support_layer_id = layer_id;
        max_layer = std::max(max_support_layer_id - int(config.z_distance_top_layers), 0);
    }
    if (max_layer > 0)
        // The actual precalculation happens in TreeModelVolumes.
        volumes.precalculate(*print.get_object(object_ids.front()), max_layer, throw_on_cancel);
    return max_layer;
}

// picked from convert_lines_to_internal()
[[nodiscard]] LineStatus get_avoidance_status(const Point& p, coord_t radius, LayerIndex layer_idx,
    const TreeModelVolumes& volumes, const TreeSupportSettings& config)
{
    const bool min_xy_dist = config.xy_distance >= config.xy_min_distance;

    LineStatus type = LineStatus::INVALID;

    if (!contains(volumes.getAvoidance(radius, layer_idx, TreeModelVolumes::AvoidanceType::FastSafe, false, min_xy_dist), p))
        type = LineStatus::TO_BP_SAFE;
    else if (!contains(volumes.getAvoidance(radius, layer_idx, TreeModelVolumes::AvoidanceType::Fast, false, min_xy_dist), p))
        type = LineStatus::TO_BP;
    else if (config.support_rests_on_model && !contains(volumes.getAvoidance(radius, layer_idx, TreeModelVolumes::AvoidanceType::FastSafe, true, min_xy_dist), p))
        type = LineStatus::TO_MODEL_GRACIOUS_SAFE;
    else if (config.support_rests_on_model && !contains(volumes.getAvoidance(radius, layer_idx, TreeModelVolumes::AvoidanceType::Fast, true, min_xy_dist), p))
        type = LineStatus::TO_MODEL_GRACIOUS;
    else if (config.support_rests_on_model && !contains(volumes.getCollision(radius, layer_idx, min_xy_dist), p))
        type = LineStatus::TO_MODEL;

    return type;
}

/*!
 * \brief Converts a Polygons object representing a line into the internal format.
 *
 * \param polylines[in] The Polyline that will be converted.
 * \param layer_idx[in] The current layer.
 * \return All lines of the \p polylines object, with information for each point regarding in which avoidance it is currently valid in.
 */
// Called by generate_initial_areas()
[[nodiscard]] static LineInformations convert_lines_to_internal(
    const TreeModelVolumes &volumes, const TreeSupportSettings &config,
    const Polylines &polylines, LayerIndex layer_idx)
{
    const bool min_xy_dist = config.xy_distance >= config.xy_min_distance;

    LineInformations result;
    // Also checks if the position is valid, if it is NOT, it deletes that point
    for (const Polyline &line : polylines) {
        LineInformation res_line;
        for (Point p : line) {
            if (! contains(volumes.getAvoidance(config.getRadius(0), layer_idx, TreeModelVolumes::AvoidanceType::FastSafe, false, min_xy_dist), p))
                res_line.emplace_back(p, LineStatus::TO_BP_SAFE);
            else if (! contains(volumes.getAvoidance(config.getRadius(0), layer_idx, TreeModelVolumes::AvoidanceType::Fast, false, min_xy_dist), p))
                res_line.emplace_back(p, LineStatus::TO_BP);
            else if (config.support_rests_on_model && ! contains(volumes.getAvoidance(config.getRadius(0), layer_idx, TreeModelVolumes::AvoidanceType::FastSafe, true, min_xy_dist), p))
                res_line.emplace_back(p, LineStatus::TO_MODEL_GRACIOUS_SAFE);
            else if (config.support_rests_on_model && ! contains(volumes.getAvoidance(config.getRadius(0), layer_idx, TreeModelVolumes::AvoidanceType::Fast, true, min_xy_dist), p))
                res_line.emplace_back(p, LineStatus::TO_MODEL_GRACIOUS);
            else if (config.support_rests_on_model && ! contains(volumes.getCollision(config.getRadius(0), layer_idx, min_xy_dist), p))
                res_line.emplace_back(p, LineStatus::TO_MODEL);
            else if (!res_line.empty()) {
                result.emplace_back(res_line);
                res_line.clear();
            }
        }
        if (!res_line.empty()) {
            result.emplace_back(res_line);
            res_line.clear();
        }
    }

    validate_range(result);
    return result;
}

// Contact candidates are generated directly below the model. Use the local
// tip clearance for the model test instead of the ordinary support XY gap;
// otherwise the candidates that are meant to carry a contact roof are
// discarded before they can become tree tips.
[[nodiscard]] static LineInformations convert_contact_lines_to_internal(
    const TreeModelVolumes &volumes, const TreeSupportSettings &config,
    const Polylines &polylines, LayerIndex layer_idx)
{
    const bool min_xy_dist = config.xy_distance >= config.xy_min_distance;
    const coord_t collision_radius = config.getRadius(0);
    // Radius, layer and local clearances are identical for every sampled point.
    // Retain only this batch's polygon instead of rebuilding it for each point.
    const Polygons contact_collision = config.support_rests_on_model && !polylines.empty() ?
        volumes.getCollisionForContact(collision_radius, layer_idx,
            config.support_tip_xy_distance, config.support_tip_z_distance) : Polygons{};

    LineInformations result;
    for (const Polyline &line : polylines) {
        LineInformation res_line;
        for (Point p : line) {
            if (config.support_rests_on_model && ! contains(contact_collision, p))
                res_line.emplace_back(p, LineStatus::CONTACT_TO_MODEL);
            else if (!config.support_rests_on_model && ! contains(
                    volumes.getAvoidance(collision_radius, layer_idx,
                        TreeModelVolumes::AvoidanceType::FastSafe, false, min_xy_dist), p))
                res_line.emplace_back(p, LineStatus::TO_BP_SAFE);
            else if (!config.support_rests_on_model && ! contains(
                    volumes.getAvoidance(collision_radius, layer_idx,
                        TreeModelVolumes::AvoidanceType::Fast, false, min_xy_dist), p))
                res_line.emplace_back(p, LineStatus::TO_BP);
            else if (!res_line.empty()) {
                result.emplace_back(std::move(res_line));
                res_line.clear();
            }
        }
        if (!res_line.empty())
            result.emplace_back(std::move(res_line));
    }
    validate_range(result);
    return result;
}

#if 0
/*!
 * \brief Converts lines in internal format into a Polygons object representing these lines.
 *
 * \param lines[in] The lines that will be converted.
 * \return All lines of the \p lines object as a Polygons object.
 */
[[nodiscard]] static Polylines convert_internal_to_lines(LineInformations lines)
{
    Polylines result;
    for (LineInformation line : lines) {
        Polyline path;
        for (auto point_data : line)
            path.points.emplace_back(point_data.first);
        result.emplace_back(std::move(path));
    }
    validate_range(result);
    return result;
}
#endif

/*!
 * \brief Evaluates if a point has to be added now. Required for a split_lines call in generate_initial_areas().
 *
 * \param current_layer[in] The layer on which the point lies, point and its status.
 * \return whether the point is valid.
 */
[[nodiscard]] static bool evaluate_point_for_next_layer_function(
    const TreeModelVolumes &volumes, const TreeSupportSettings &config,
    size_t current_layer, const std::pair<Point, LineStatus> &p,
    std::optional<Polygons> &contact_collision)
{
    using AvoidanceType = TreeModelVolumes::AvoidanceType;
    const bool min_xy_dist = config.xy_distance >= config.xy_min_distance;
    if (! contains(volumes.getAvoidance(config.getRadius(0), current_layer - 1, p.second == LineStatus::TO_BP_SAFE ? AvoidanceType::FastSafe : AvoidanceType::Fast, false, min_xy_dist), p.first))
        return true;
    if (config.support_rests_on_model && (p.second != LineStatus::TO_BP && p.second != LineStatus::TO_BP_SAFE)) {
        if (p.second == LineStatus::CONTACT_TO_MODEL) {
            if (!contact_collision)
                contact_collision = volumes.getCollisionForContact(config.getRadius(0), current_layer - 1,
                    config.support_tip_xy_distance, config.support_tip_z_distance);
            return ! contains(*contact_collision, p.first);
        }
        // Both alternatives return references. Keep the contact value branch
        // separate so the conditional does not copy the ordinary polygons.
        const Polygons &forbidden =
            p.second == LineStatus::TO_MODEL_GRACIOUS || p.second == LineStatus::TO_MODEL_GRACIOUS_SAFE ?
                volumes.getAvoidance(config.getRadius(0), current_layer - 1, p.second == LineStatus::TO_MODEL_GRACIOUS_SAFE ? AvoidanceType::FastSafe : AvoidanceType::Fast, true, min_xy_dist) :
                volumes.getCollision(config.getRadius(0), current_layer - 1, min_xy_dist);
        return ! contains(forbidden, p.first);
    }
    return false;
}

/*!
 * \brief Evaluates which points of some lines are not valid one layer below and which are. Assumes all points are valid on the current layer. Validity is evaluated using supplied lambda.
 *
 * \param lines[in] The lines that have to be evaluated.
 * \param evaluatePoint[in] The function used to evaluate the points.
 * \return A pair with which points are still valid in the first slot and which are not in the second slot.
 */
template<typename EvaluatePointFn>
[[nodiscard]] static std::pair<LineInformations, LineInformations> split_lines(const LineInformations &lines, EvaluatePointFn evaluatePoint)
{
    // assumes all Points on the current line are valid

    LineInformations keep;
    LineInformations set_free;
    for (const std::vector<std::pair<Point, LineStatus>> &line : lines) {
        bool            current_keep = true;
        LineInformation resulting_line;
        for (const std::pair<Point, LineStatus> &me : line) {
            if (evaluatePoint(me) != current_keep) {
                if (! resulting_line.empty())
                    (current_keep ? &keep : &set_free)->emplace_back(std::move(resulting_line));
                current_keep = !current_keep;
            }
            resulting_line.emplace_back(me);
        }
        if (! resulting_line.empty())
            (current_keep ? &keep : &set_free)->emplace_back(std::move(resulting_line));
    }
    validate_range(keep);
    validate_range(set_free);
    return std::pair<std::vector<std::vector<std::pair<Point, LineStatus>>>, std::vector<std::vector<std::pair<Point, LineStatus>>>>(keep, set_free);
}

// Ported from CURA's PolygonUtils::getNextPointWithDistance()
// Sample a next point at distance "dist" from start_pt on polyline segment (start_idx, start_idx + 1).
// Returns sample point and start index of its segment on polyline if such sample exists.
static std::optional<std::pair<Point, size_t>> polyline_sample_next_point_at_distance(const Points &polyline, const Point &start_pt, size_t start_idx, double dist)
{
    const double                dist2  = sqr(dist);
    const auto                  dist2i = int64_t(dist2);
    const auto eps    = scaled<double>(0.01);

    for (size_t i = start_idx + 1; i < polyline.size(); ++ i) {
        const Point p1 = polyline[i];
        if ((p1 - start_pt).cast<int64_t>().squaredNorm() >= dist2i) {
            // The end point is outside the circle with center "start_pt" and radius "dist".
            const Point p0  = polyline[i - 1];
            Vec2d       v   = (p1 - p0).cast<double>();
            double      l2v = v.squaredNorm();
            if (l2v < sqr(eps)) {
                // Very short segment.
                Point c = (p0 + p1) / 2;
                if (std::abs((start_pt - c).cast<double>().norm() - dist) < eps)
                    return std::pair<Point, size_t>{ c, i - 1 };
                else
                    continue;
            }
            Vec2d p0f = (start_pt - p0).cast<double>();
            // Foot point of start_pt into v.
            Vec2d foot_pt = v * (p0f.dot(v) / l2v);
            // Vector from foot point of "start_pt" to "start_pt".
            Vec2d xf = p0f - foot_pt;
            // Squared distance of "start_pt" from the ray (p0, p1).
            double l2_from_line = xf.squaredNorm();
            // Squared distance of an intersection point of a circle with center at the foot point.
            if (double l2_intersection = dist2 - l2_from_line;
                l2_intersection > - SCALED_EPSILON) {
                // The ray (p0, p1) touches or intersects a circle centered at "start_pt" with radius "dist".
                // Distance of the circle intersection point from the foot point.
                l2_intersection = std::max(l2_intersection, 0.);
                if ((v - foot_pt).cast<double>().squaredNorm() >= l2_intersection) {
                    // Intersection of the circle with the segment (p0, p1) is on the right side (close to p1) from the foot point.
                    Point p = p0 + (foot_pt + v * sqrt(l2_intersection / l2v)).cast<coord_t>();
                    validate_range(p);
                    return std::pair<Point, size_t>{ p, i - 1 };
                }
            }
        }
    }
    return {};
}

/*!
 * \brief Eensures that every line segment is about distance in length. The resulting lines may differ from the original but all points are on the original
 *
 * \param input[in] The lines on which evenly spaced points should be placed.
 * \param distance[in] The distance the points should be from each other.
 * \param min_points[in] The amount of points that have to be placed. If not enough can be placed the distance will be reduced to place this many points.
 * \return A Polygons object containing the evenly spaced points. Does not represent an area, more a collection of points on lines.
 */
[[nodiscard]] static Polylines ensure_maximum_distance_polyline(const Polylines &input, double distance, size_t min_points)
{
    Polylines result;
    for (Polyline part : input) {
        if (part.empty())
            continue;

        double len = length(part.points);
        Polyline line;
        double current_distance = std::max(distance, scaled<double>(0.1));
        if (len < 2 * distance && min_points <= 1)
        {
            // Insert the opposite point of the first one.
            //FIXME pretty expensive
            Polyline pl(part);
            pl.clip_end(len / 2);
            line.points.emplace_back(pl.points.back());
        }
        else if (part.size() == 2) {
            // Keep both ends supported, distributing the remainder over all
            // intervals instead of leaving one short interval at the far end.
            const size_t intervals = std::max<size_t>(
                std::max<size_t>(1, min_points > 0 ? min_points - 1 : 0),
                size_t(std::ceil(len / current_distance)));
            for (size_t i = 0; i <= intervals; ++i)
                line.points.emplace_back(part.front() +
                    ((part.back() - part.front()).cast<double>() * (double(i) / double(intervals))).cast<coord_t>());
        }
        else
        {
            size_t optimal_end_index = part.size() - 1;

            if (part.front() == part.back()) {
                size_t optimal_start_index = 0;
                // If the polyline was a polygon, there is a high chance it was an overhang. Overhangs that are <60� tend to be very thin areas, so lets get the beginning and end of them and ensure that they are supported.
                // The first point of the line will always be supported, so rotate the order of points in this polyline that one of the two corresponding points that are furthest from each other is in the beginning.
                // The other will be manually added (optimal_end_index)
                coord_t max_dist2_between_vertecies = 0;
                for (size_t idx = 0; idx < part.size() - 1; ++ idx) {
                    for (size_t inner_idx = 0; inner_idx < part.size() - 1; inner_idx++) {
                        if ((part[idx] - part[inner_idx]).cast<double>().squaredNorm() > max_dist2_between_vertecies) {
                            optimal_start_index = idx;
                            optimal_end_index = inner_idx;
                            max_dist2_between_vertecies = (part[idx] - part[inner_idx]).cast<double>().squaredNorm();
                        }
                    }
                }
                std::rotate(part.begin(), part.begin() + optimal_start_index, part.end() - 1);
                part[part.size() - 1] = part[0]; // restore that property that this polyline ends where it started.
                optimal_end_index = (part.size() + optimal_end_index - optimal_start_index - 1) % (part.size() - 1);
            }

            while (line.size() < min_points && current_distance >= scaled<double>(0.1))
            {
                line.clear();
                Point current_point = part[0];
                line.points.emplace_back(part[0]);
                if (min_points > 1 || (part[0] - part[optimal_end_index]).cast<double>().norm() > current_distance)
                    line.points.emplace_back(part[optimal_end_index]);
                size_t current_index = 0;
                std::optional<std::pair<Point, size_t>> next_point;
                double next_distance = current_distance;
                // Get points so that at least min_points are added and they each are current_distance away from each other. If that is impossible, decrease current_distance a bit.
                // The input are lines, that means that the line from the last to the first vertex does not have to exist, so exclude all points that are on this line!
                while ((next_point = polyline_sample_next_point_at_distance(part.points, current_point, current_index, next_distance))) {
                    // Not every point that is distance away, is valid, as it may be much closer to another point. This is especially the case when the overhang is very thin.
                    // So this ensures that the points are actually a certain distance from each other.
                    // This assurance is only made on a per polygon basis, as different but close polygon may not be able to use support below the other polygon.
                    double min_distance_to_existing_point = std::numeric_limits<double>::max();
                    for (Point p : line)
                        min_distance_to_existing_point = std::min(min_distance_to_existing_point, (p - next_point->first).cast<double>().norm());
                    if (min_distance_to_existing_point >= current_distance) {
                        // viable point was found. Add to possible result.
                        line.points.emplace_back(next_point->first);
                        current_point = next_point->first;
                        current_index = next_point->second;
                        next_distance = current_distance;
                    } else {
                        if (current_point == next_point->first) {
                            // In case a fixpoint is encountered, better aggressively overcompensate so the code does not become stuck here...
                            BOOST_LOG_TRIVIAL(warning) << "Tree Support: Encountered a fixpoint in polyline_sample_next_point_at_distance. This is expected to happen if the distance (currently " << next_distance <<
                                ") is smaller than 100";
                            tree_supports_show_error("Encountered issue while placing tips. Some tips may be missing."sv, true);
                            if (next_distance > 2 * current_distance)
                                // This case should never happen, but better safe than sorry.
                                break;
                            next_distance += current_distance;
                            continue;
                        }
                        // if the point was too close, the next possible viable point is at least distance-min_distance_to_existing_point away from the one that was just checked.
                        next_distance = std::max(current_distance - min_distance_to_existing_point, scaled<double>(0.1));
                        current_point = next_point->first;
                        current_index = next_point->second;
                    }
                }
                current_distance *= 0.9;
            }
        }
        result.emplace_back(std::move(line));
    }
    validate_range(result);
    return result;
}

// These are guides for tree-head sampling, never extrusion paths. Their phase
// belongs to the connected roof, not the printer origin or alternating infill
// angle. Otherwise two identical edges acquire different head positions just
// because they intersect the global infill lattice at different coordinates.
Polylines organic_roof_tip_positions(const Polygons &area, coord_t distance)
{
    Polylines result;
    if (distance <= 0)
        return result;
    for (ExPolygon component : union_ex(area)) {
        const Points &contour = component.contour.points;
        double longest = -1.;
        Vec2d axis(0., 1.);
        for (size_t i = 0; i < contour.size(); ++i) {
            Vec2d edge = (contour[(i + 1) % contour.size()] - contour[i]).cast<double>();
            const double length2 = edge.squaredNorm();
            if (length2 > longest) {
                longest = length2;
                axis = edge;
            }
        }
        const double rotation = M_PI_2 - std::atan2(axis.y(), axis.x());
        const Point origin = get_extents(component).center();
        component.translate(-origin);
        component.rotate(rotation);
        const BoundingBox bounds = get_extents(component);
        const size_t columns = std::max<size_t>(1, size_t(std::ceil(double(bounds.size().x()) / double(distance))));
        Polylines guides;
        guides.reserve(columns);
        for (size_t column = 0; column < columns; ++column) {
            const coord_t x = bounds.min.x() + coord_t(double(bounds.size().x()) * (double(column) + 0.5) / double(columns));
            guides.emplace_back(Points{Point(x, bounds.min.y() - 1), Point(x, bounds.max.y() + 1)});
        }
        Polylines samples = ensure_maximum_distance_polyline(intersection_pl(guides, component), double(distance), 1);
        for (Polyline &sample : samples) {
            sample.rotate(-rotation);
            sample.translate(origin);
            result.push_back(std::move(sample));
        }
    }
    return result;
}

/*!
 * \brief Returns Polylines representing the (infill) lines that will result in slicing the given area
 *
 * \param area[in] The area that has to be filled with infill.
 * \param roof[in] Whether the roofing or regular support settings should be used.
 * \param layer_idx[in] The current layer index.
 * \param support_infill_distance[in] The distance that should be between the infill lines.
 *
 * \return A Polygons object that represents the resulting infill lines.
 */
[[nodiscard]] static Polylines generate_support_infill_lines(
    // Polygon to fill in with a zig-zag pattern supporting an overhang.
    const Polygons          &polygon,
    const SupportParameters &support_params,
    bool roof, LayerIndex layer_idx, coord_t support_infill_distance)
{
#if 0
    Polygons gaps;
    // as we effectivly use lines to place our supportPoints we may use the Infill class for it, while not made for it it works perfect

    const EFillMethod pattern = roof ? config.roof_pattern : config.support_pattern;

//    const bool zig_zaggify_infill = roof ? pattern == EFillMethod::ZIG_ZAG : config.zig_zaggify_support;
    const bool connect_polygons = false;
    constexpr coord_t support_roof_overlap = 0;
    constexpr size_t infill_multiplier = 1;
    constexpr coord_t outline_offset = 0;
    const int support_shift = roof ? 0 : support_infill_distance / 2;
    const size_t wall_line_count = include_walls && !roof ? config.support_wall_count : 0;
    const Point infill_origin;
    constexpr Polygons* perimeter_gaps = nullptr;
    constexpr bool use_endpieces = true;
    const bool connected_zigzags = roof ? false : config.connect_zigzags;
    const size_t zag_skip_count = roof ? 0 : config.zag_skip_count;
    constexpr coord_t pocket_size = 0;
    std::vector<AngleRadians> angles = roof ? config.support_roof_angles : config.support_infill_angles;
    std::vector<VariableWidthLines> toolpaths;

    const coord_t z = config.getActualZ(layer_idx);
    int divisor = static_cast<int>(angles.size());
    int index = ((layer_idx % divisor) + divisor) % divisor;
    const AngleRadians fill_angle = angles[index];
    Infill roof_computation(pattern, true /* zig_zaggify_infill */, connect_polygons, polygon,
        roof ? config.support_roof_line_width : config.support_line_width, support_infill_distance, support_roof_overlap, infill_multiplier,
        fill_angle, z, support_shift, config.resolution, wall_line_count, infill_origin,
        perimeter_gaps, connected_zigzags, use_endpieces, false /* skip_some_zags */, zag_skip_count, pocket_size);
    Polygons polygons;
    Polygons lines;
    roof_computation.generate(toolpaths, polygons, lines, config.settings);
    append(lines, to_polylines(polygons));
    return lines;
#else
//    const bool connected_zigzags = roof ? false : config.connect_zigzags;
//    const int support_shift = roof ? 0 : support_infill_distance / 2;

    const Flow            &flow   = roof ? support_params.support_material_interface_flow : support_params.support_material_flow;
    std::unique_ptr<Fill>  filler = std::unique_ptr<Fill>(Fill::new_from_type(roof ? support_params.interface_fill_pattern : support_params.base_fill_pattern));
    FillParams             fill_params;

    filler->layer_id = layer_idx;
    filler->spacing  = flow.spacing();
    filler->angle = roof ?
        //fixme support_layer.interface_id() instead of layer_idx
        (support_params.interface_angle + ((layer_idx & 1) ? float(- M_PI_4) : float(+ M_PI_4))) :
        support_params.base_angle;

    // INLONG: use top-specific interface density after separating top/bottom settings.
    fill_params.density     = float(roof ? support_params.top_interface_density : scaled<float>(filler->spacing) / (scaled<float>(filler->spacing) + float(support_infill_distance)));
    fill_params.dont_adjust = true;

    Polylines out;
    for (ExPolygon &expoly : union_ex(polygon)) {
        // The surface type does not matter.
        assert(area(expoly) > 0.);
#ifdef TREE_SUPPORT_SHOW_ERRORS_WIN32
        if (area(expoly) <= 0.)
            ::MessageBoxA(nullptr, "TreeSupport infill negative area", "Bug detected!", MB_OK | MB_SYSTEMMODAL | MB_SETFOREGROUND | MB_ICONWARNING);
#endif // TREE_SUPPORT_SHOW_ERRORS_WIN32
        assert(intersecting_edges(to_polygons(expoly)).empty());
        check_self_intersections(expoly, "generate_support_infill_lines");
        Surface surface(stInternal, std::move(expoly));
        try {
            Polylines pl = filler->fill_surface(&surface, fill_params);
            assert(pl.empty() || get_extents(surface.expolygon).inflated(SCALED_EPSILON).contains(get_extents(pl)));
#ifdef TREE_SUPPORT_SHOW_ERRORS_WIN32
            if (! pl.empty() && ! get_extents(surface.expolygon).inflated(SCALED_EPSILON).contains(get_extents(pl)))
                ::MessageBoxA(nullptr, "TreeSupport infill failure", "Bug detected!", MB_OK | MB_SYSTEMMODAL | MB_SETFOREGROUND | MB_ICONWARNING);
#endif // TREE_SUPPORT_SHOW_ERRORS_WIN32
            append(out, std::move(pl));
        } catch (InfillFailedException &) {
        }
    }
    validate_range(out);
    return out;
#endif
}

/*!
 * \brief Unions two Polygons. Ensures that if the input is non empty that the output also will be non empty.
 * \param first[in] The first Polygon.
 * \param second[in] The second Polygon.
 * \return The union of both Polygons
 */
[[nodiscard]] static Polygons safe_union(const Polygons first, const Polygons second = {})
{
    // unionPolygons can slowly remove Polygons under certain circumstances, because of rounding issues (Polygons that have a thin area).
    // This does not cause a problem when actually using it on large areas, but as influence areas (representing centerpoints) can be very thin, this does occur so this ugly workaround is needed
    // Here is an example of a Polygons object that will loose vertices when unioning, and will be gone after a few times unionPolygons was called:
    /*
    Polygons example;
    Polygon exampleInner;
    exampleInner.add(Point(120410,83599));//A
    exampleInner.add(Point(120384,83643));//B
    exampleInner.add(Point(120399,83618));//C
    exampleInner.add(Point(120414,83591));//D
    exampleInner.add(Point(120423,83570));//E
    exampleInner.add(Point(120419,83580));//F
    example.add(exampleInner);
    for(int i=0;i<10;i++){
         log("Iteration %d Example area: %f\n",i,area(example));
         example=example.unionPolygons();
    }
*/

    Polygons result;
    if (! first.empty() || ! second.empty()) {
        result = union_(first, second);
        if (result.empty()) {
            BOOST_LOG_TRIVIAL(debug) << "Caught an area destroying union, enlarging areas a bit.";
            // just take the few lines we have, and offset them a tiny bit. Needs to be offsetPolylines, as offset may aleady have problems with the area.
            result = union_(offset(to_polylines(first), scaled<float>(0.002), jtMiter, 1.2), offset(to_polylines(second), scaled<float>(0.002), jtMiter, 1.2));
        }
    }

    return result;
}

/*!
 * \brief Offsets (increases the area of) a polygons object in multiple steps to ensure that it does not lag through over a given obstacle.
 * \param me[in] Polygons object that has to be offset.
 * \param distance[in] The distance by which me should be offset. Expects values >=0.
 * \param collision[in] The area representing obstacles.
 * \param last_step_offset_without_check[in] The most it is allowed to offset in one step.
 * \param min_amount_offset[in] How many steps have to be done at least. As this uses round offset this increases the amount of vertices, which may be required if Polygons get very small. Required as arcTolerance is not exposed in offset, which should result with a similar result.
 * \return The resulting Polygons object.
 */
[[nodiscard]] static Polygons safe_offset_inc(const Polygons& me, coord_t distance, const Polygons& collision, coord_t safe_step_size, coord_t last_step_offset_without_check, size_t min_amount_offset)
{
    bool do_final_difference = last_step_offset_without_check == 0;
    Polygons ret = safe_union(me); // ensure sane input

    // Trim the collision polygons with the region of interest for diff() efficiency.
    Polygons collision_trimmed_buffer;
    auto collision_trimmed = [&collision_trimmed_buffer, &collision, &ret, distance]() -> const Polygons& {
        if (collision_trimmed_buffer.empty() && ! collision.empty())
            collision_trimmed_buffer = ClipperUtils::clip_clipper_polygons_with_subject_bbox(collision, get_extents(ret).inflated(std::max((coord_t)0, distance) + SCALED_EPSILON));
        return collision_trimmed_buffer;
    };

    if (distance == 0)
        return do_final_difference ? diff(ret, collision_trimmed()) : union_(ret);
    if (safe_step_size < 0 || last_step_offset_without_check < 0) {
        BOOST_LOG_TRIVIAL(warning) << "Offset increase got invalid parameter!";
        tree_supports_show_error("Negative offset distance... How did you manage this ?"sv, true);
        return do_final_difference ? diff(ret, collision_trimmed()) : union_(ret);
    }

    coord_t step_size = safe_step_size;
    int     steps = distance > last_step_offset_without_check ? (distance - last_step_offset_without_check) / step_size : 0;
    if (distance - steps * step_size > last_step_offset_without_check) {
        if ((steps + 1) * step_size <= distance)
            // This will be the case when last_step_offset_without_check >= safe_step_size
            ++ steps;
        else
            do_final_difference = true;
    }
    if (steps + (distance < last_step_offset_without_check || (distance % step_size) != 0) < int(min_amount_offset) && min_amount_offset > 1) {
        // yes one can add a bool as the standard specifies that a result from compare operators has to be 0 or 1
        // reduce the stepsize to ensure it is offset the required amount of times
        step_size = distance / min_amount_offset;
        if (step_size >= safe_step_size) {
            // effectivly reduce last_step_offset_without_check
            step_size = safe_step_size;
            steps = min_amount_offset;
        } else
            steps = distance / step_size;
    }
    // offset in steps
    for (int i = 0; i < steps; ++ i) {
        ret = diff(offset(ret, step_size, jtRound, scaled<float>(0.01)), collision_trimmed());
        // ensure that if many offsets are done the performance does not suffer extremely by the new vertices of jtRound.
        if (i % 10 == 7)
            ret = polygons_simplify(ret, scaled<double>(0.015));
    }
    // offset the remainder
    float last_offset = distance - steps * step_size;
    if (last_offset > SCALED_EPSILON)
        ret = offset(ret, distance - steps * step_size, jtRound, scaled<float>(0.01));
    ret = polygons_simplify(ret, scaled<double>(0.015));

    if (do_final_difference)
        ret = diff(ret, collision_trimmed());
    return union_(ret);
}

// Build a small footprint around sharp convex contour tips. Polygon::convex_points()
// measures the exterior turn between the incoming and outgoing contour edges.
// The UI exposes the equivalent interior tip angle, so a 45 degree setting is
// passed here as a 135 degree exterior-turn threshold.
[[nodiscard]] static Polygons sharp_tip_contact_mask(const Polygons &areas,
                                                     double turn_angle,
                                                     coord_t radius,
                                                     const Polygons *support_areas = nullptr,
                                                     bool extend_incident_edges = true)
{
    if (areas.empty() || radius <= 0 || turn_angle >= M_PI - EPSILON)
        return {};

    const double tip_radius = unscale<double>(radius);
    // The raw overhang difference is the only source allowed to use the
    // narrow-strip fallback.  A model slice can contain many thin vertical
    // walls that are unrelated to the active overhang; classifying those
    // walls as tip contacts was what produced the detached interface islands.
    const bool allow_narrow_strip = support_areas == nullptr;
    const double narrow_width_limit = std::max(4.0 * tip_radius, 1.5);
    // The overhang detector works on a difference between adjacent model
    // slices.  At a sharp edge that difference may contain only one incident
    // side, even though the model contour still has the complete corner.  Use
    // the same local reach used by the tip roof to decide whether a model tip
    // belongs to this overhang; do not use the whole model slice, otherwise
    // every convex corner would receive a tip contact footprint.
    Polygons support_adjacency;
    if (support_areas != nullptr && !support_areas->empty()) {
        // A tip is associated with the overhang that actually caused it, not
        // with every contour corner in a large neighbourhood.  The old
        // 20-radius envelope promoted unrelated wall corners into contact
        // platforms and was the source of detached interface islands.  Keep
        // the envelope to one tip footprint plus one extrusion width; the
        // ordinary overhang footprint still supplies the full platform.
        const coord_t adjacency_radius = radius + scaled<coord_t>(0.5);
        support_adjacency = offset(union_ex(*support_areas), adjacency_radius,
            jtRound, SUPPORT_TREE_CIRCLE_RESOLUTION);
    }

    Polygons mask;
    for (const Polygon &poly : areas) {
        if (poly.points.size() < 3)
            continue;
        // Polygon::convex_points() intentionally excludes an exact threshold
        // angle and advances its own threshold by one ULP internally. A
        // single nextafter() therefore cancels out at an exact right angle.
        // Keep a small explicit tolerance so the UI's inclusive 90° setting
        // really includes a mathematically exact 90° corner.
        const double inclusive_turn_angle = turn_angle > 1e-6 ?
            turn_angle - 1e-6 : 0.0;
        const Points convex_tips = poly.convex_points(inclusive_turn_angle);
        for (const Point &tip : convex_tips) {
            if (!support_adjacency.empty() && !contains(support_adjacency, tip))
                continue;
            Polygon circle{ make_circle(radius, SUPPORT_TREE_CIRCLE_RESOLUTION) };
            circle.translate(tip);
            mask.emplace_back(std::move(circle));

            // A vertex-only circle leaves the two neighboring contact strips
            // disconnected when the overhang detector trims the last few
            // millimeters before the vertex. Extend the two incident contour
            // edges as one narrow V-shaped platform. When only one incident
            // edge is present in the overhang area, keep the wrap short so a
            // one-sided corner does not grow a long unsupported cap.
            auto tip_it = std::find(poly.points.begin(), poly.points.end(), tip);
            if (extend_incident_edges && tip_it != poly.points.end() && poly.points.size() >= 3) {
                const size_t tip_idx = size_t(std::distance(poly.points.begin(), tip_it));
                const Point  previous = poly.points[(tip_idx + poly.points.size() - 1) % poly.points.size()];
                const Point  next     = poly.points[(tip_idx + 1) % poly.points.size()];
                const coord_t long_extension = scaled<coord_t>(std::max(4.0, 20.0 * tip_radius));
                // The V is only a local bridge.  The previous implementation
                // treated the raw overhang itself as proof that both sides
                // were supported, so a one-sided tip frequently received a
                // 4--8 mm V.  Continuity between two supported sides is
                // provided by the collision-clipped contact footprint and its
                // fill paths; the incident-edge bridge must stay within the
                // user-visible 0.5 mm limit in every case.
                const coord_t extension = std::min(long_extension, scaled<coord_t>(0.5));
                auto limit_to_extension = [tip, extension](const Point &from) {
                    const Vec2d direction = (from - tip).cast<double>();
                    const double length = direction.norm();
                    if (length <= EPSILON)
                        return tip;
                    const double distance = std::min<double>(double(extension), length);
                    return Point(tip + (direction * (distance / length)).cast<coord_t>());
                };
                const Polyline incident_edges({ limit_to_extension(previous), tip, limit_to_extension(next) });
                append(mask, offset(incident_edges, radius, jtRound, SUPPORT_TREE_CIRCLE_RESOLUTION));
            }
        }

        if (allow_narrow_strip) {
            const double perimeter = poly.length();
            const double polygon_area = std::abs(poly.area());
            const double hydraulic_width = perimeter > EPSILON ? 2. * polygon_area / perimeter : std::numeric_limits<double>::infinity();
            const double contour_length = unscale<double>(perimeter);
            if (poly.area() > 0. && contour_length > 4. * tip_radius && hydraulic_width <= scaled<double>(narrow_width_limit)) {
                // This is a property of the overhang difference itself, not
                // of an arbitrary nearby model wall.  Keep the wrap local so
                // a sharp edge still gets a printable platform without the
                // former long necklace around unrelated contours.
                const coord_t contour_wrap = std::min<coord_t>(radius, scaled<coord_t>(0.5));
                append(mask, offset(poly, contour_wrap, jtRound, SUPPORT_TREE_CIRCLE_RESOLUTION));
            }
        }

    }
    return mask.empty() ? Polygons{} : union_(mask);
}

class RichInterfacePlacer : public InterfacePlacer {
public:
    // Each overhang layer computes its own output in parallel. Publishing in
    // source-layer order makes cross-layer tip deduplication deterministic.
    struct PendingPlacement {
        struct Roof {
            Polygons polygons;
            size_t   layer_idx;
            size_t   dtt_roof;
        };
        struct Tip {
            std::pair<Point, LineStatus> point;
            LayerIndex layer_idx;
            size_t     dont_move_until;
            bool       roof;
            size_t     roof_recovery_dtt;
            bool       skip_ovalisation;
            bool       roof_already_generated;
        };
        std::vector<Roof> roofs;
        std::vector<Tip>  tips;
    };

    RichInterfacePlacer(
        const InterfacePlacer        &interface_placer,
        const TreeModelVolumes       &volumes,
        bool                          force_tip_to_roof,
        size_t                        num_support_layers,
        std::vector<SupportElements> &move_bounds,
        PendingPlacement            *pending = nullptr)
    :
        InterfacePlacer(interface_placer),
        volumes(volumes), force_tip_to_roof(force_tip_to_roof), move_bounds(move_bounds), m_pending(pending)
    {
        if (m_pending == nullptr)
            m_already_inserted.assign(num_support_layers, {});
        this->min_xy_dist = this->config.xy_distance >= this->config.xy_min_distance;
        m_base_radius = scaled<coord_t>(0.01);
        m_base_circle = Polygon{ make_circle(m_base_radius, SUPPORT_TREE_CIRCLE_RESOLUTION) };

    }
    const TreeModelVolumes                             &volumes;
    // Radius of the tree tip is large enough to be covered by an interface.
    const bool                                          force_tip_to_roof;
    bool                                                min_xy_dist;

public:
    void add_roof(Polygons &&polygons, size_t layer_idx, size_t dtt_roof)
    {
        if (m_pending != nullptr) {
            m_pending->roofs.push_back({std::move(polygons), layer_idx, dtt_roof});
        } else {
            InterfacePlacer::add_roof(std::move(polygons), layer_idx, dtt_roof);
        }
    }

    void add_roofs(std::vector<Polygons> &&roofs, size_t layer_idx)
    {
        for (size_t idx = 0; idx < roofs.size(); ++idx)
            if (!roofs[idx].empty())
                this->add_roof(std::move(roofs[idx]), layer_idx - idx, idx);
    }

    void add_roof_build_plate(Polygons &&polygons, size_t dtt_roof)
    {
        this->add_roof(std::move(polygons), 0,
            std::min(dtt_roof, this->support_parameters.num_top_interface_layers));
    }

    void publish_roofs(PendingPlacement &pending)
    {
        assert(m_pending == nullptr);
        for (PendingPlacement::Roof &roof : pending.roofs)
            this->add_roof(std::move(roof.polygons), roof.layer_idx, roof.dtt_roof);
    }

    void publish_tips(PendingPlacement &&pending)
    {
        assert(m_pending == nullptr);
        const SupportGeneratorLayersPtr &contacts = this->top_contacts_mutable();
        for (const PendingPlacement::Tip &tip : pending.tips) {
            const size_t contact_idx = size_t(tip.layer_idx) + tip.roof_recovery_dtt;
            if (contact_idx < contacts.size() && contacts[contact_idx] != nullptr &&
                contacts[contact_idx]->polygons.empty() &&
                contacts[contact_idx]->idx_object_layer_above != size_t(-1))
                continue;
            this->add_point_as_influence_area(tip.point, tip.layer_idx, tip.dont_move_until,
                tip.roof, tip.roof_recovery_dtt, tip.skip_ovalisation, tip.roof_already_generated);
        }
        pending = PendingPlacement{};
    }

    // called by sample_overhang_area()
    void add_points_along_lines(
        // Insert points (tree tips or top contact interfaces) along these lines.
        LineInformations    lines,
        // Start at this layer.
        LayerIndex          insert_layer_idx,
        // Insert this number of interface layers.
        size_t              roof_tip_layers,
        // True if an interface is already generated above these lines.
        size_t              supports_roof_layers,
        // The element tries to not move until this dtt is reached.
        size_t              dont_move_until,
        // Optional clipped overhang region for contacts/interfaces.
        // Organic tips still place branches; roofs follow this region instead
        // of a necklace of overlapping tip circles.
        const Polygons       *continuous_roof_area = nullptr,
        // Sharp-tail contact footprint that intentionally ignores XY
        // clearance.  This remains separate from the ordinary footprint so
        // the exception cannot relax branch collision avoidance globally.
        const Polygons       *continuous_roof_area_no_xy = nullptr,
        // Configurable sharp-tip contact footprint using local XY/Z values.
        const Polygons       *continuous_roof_area_tip = nullptr)
    {
        validate_range(lines);
        // Add tip area as roof (happens when minimum roof area > minimum tip area) if possible
        size_t dtt_roof_tip;
        for (dtt_roof_tip = 0; dtt_roof_tip < roof_tip_layers && insert_layer_idx - dtt_roof_tip >= 1; ++ dtt_roof_tip) {
            size_t this_layer_idx = insert_layer_idx - dtt_roof_tip;
            const size_t roof_recovery_depth = dtt_roof_tip + supports_roof_layers;
            const bool use_continuous_roof = continuous_roof_area != nullptr || continuous_roof_area_no_xy != nullptr || continuous_roof_area_tip != nullptr;
            auto evaluateRoofWillGenerate = [&](const std::pair<Point, LineStatus> &p) {
                //FIXME Vojtech: The circle is just shifted, it has a known size, the infill should fit all the time!
    #if 0
                Polygon roof_circle;
                for (Point corner : base_circle)
                    roof_circle.points.emplace_back(p.first + corner * config.min_radius);
                return !generate_support_infill_lines({ roof_circle }, config, true, insert_layer_idx - dtt_roof_tip, config.support_roof_line_distance).empty();
    #else
                return true;
    #endif
            };

            {
                std::optional<Polygons> contact_collision;
                std::pair<LineInformations, LineInformations> split =
                    // keep all lines that are still valid on the next layer
                    split_lines(lines, [this, this_layer_idx, &contact_collision](const std::pair<Point, LineStatus> &p)
                        { return evaluate_point_for_next_layer_function(volumes, config, this_layer_idx, p, contact_collision); });
                LineInformations points = std::move(split.second);
                // Not all roofs are guaranteed to actually generate lines, so filter these out and add them as points.
                split = split_lines(split.first, evaluateRoofWillGenerate);
                lines = std::move(split.first);
                append(points, split.second);
                // add all points that would not be valid
                for (const LineInformation &line : points)
                    for (const std::pair<Point, LineStatus> &point_data : line)
                        add_point_as_influence_area(point_data, this_layer_idx,
                            // don't move until
                            roof_tip_layers - dtt_roof_tip,
                            // supports roof
                            roof_recovery_depth > 0,
                            // recovered roof/contact depth for this slice
                            roof_recovery_depth,
                            // disable ovalization
                            false,
                            // The contact polygon is generated independently
                            // of whether an individual tip can move down.
                            use_continuous_roof);
            }

            // The sampler returns a set of tips, not an ordered path: it inserts
            // the furthest endpoint before the remaining contour samples.
            // Sweeping between consecutive tips would connect across the model.
            // Keep the original per-tip regions; continuous roofs are generated
            // from the clipped overhang polygons by sample_overhang_area().
            Polygons new_roofs;
            // Contact geometry is independent of clearance and fill pattern.
            // Tree tips still synchronize/support the roof stack, while the
            // filler generates the selected pattern inside this footprint.
            if (continuous_roof_area != nullptr)
                new_roofs = diff(*continuous_roof_area, volumes.getCollision(0, this_layer_idx, min_xy_dist));
            if (continuous_roof_area_no_xy != nullptr)
                append(new_roofs, diff(*continuous_roof_area_no_xy, volumes.getCollisionWithoutXY(0, this_layer_idx)));
            if (continuous_roof_area_tip != nullptr)
                append(new_roofs, diff(*continuous_roof_area_tip,
                    volumes.getCollisionForContact(0, this_layer_idx,
                        config.support_tip_xy_distance, config.support_tip_z_distance)));
            if (!use_continuous_roof)
                for (const LineInformation &line : lines)
                    for (const std::pair<Point, LineStatus> &p : line) {
                        Polygon roof_circle{ m_base_circle };
                        roof_circle.scale(config.min_radius / m_base_radius);
                        roof_circle.translate(p.first);
                        new_roofs.emplace_back(std::move(roof_circle));
                    }
            this->add_roof(std::move(new_roofs), this_layer_idx, roof_recovery_depth);
        }

        const size_t roof_recovery_depth = dtt_roof_tip + supports_roof_layers;
        for (const LineInformation &line : lines) {
            // If a line consists of enough tips, the assumption is that it is not a single tip, but part of a simulated support pattern.
            // Ovalisation should be disabled for these to improve the quality of the lines when tip_diameter=line_width
            bool disable_ovalistation = config.min_radius < 3 * config.support_line_width && roof_tip_layers == 0 && dtt_roof_tip == 0 && line.size() > 5;
            for (const std::pair<Point, LineStatus> &point_data : line)
                add_point_as_influence_area(point_data, insert_layer_idx - dtt_roof_tip,
                    // don't move until
                    dont_move_until > dtt_roof_tip ? dont_move_until - dtt_roof_tip : 0,
                    // supports roof
                    roof_recovery_depth > 0,
                    // recovered roof/contact depth for this slice
                    roof_recovery_depth,
                    disable_ovalistation);
        }
    }

private:
    // called by this->add_points_along_lines()
    void add_point_as_influence_area(std::pair<Point, LineStatus> p, LayerIndex insert_layer, size_t dont_move_until, bool roof, size_t roof_recovery_dtt, bool skip_ovalisation, bool roof_already_generated = false)
    {
        bool to_bp = p.second == LineStatus::TO_BP || p.second == LineStatus::TO_BP_SAFE;
        // Local contact clearance only proves that a tip fits. A flat model
        // landing must be established by the ordinary avoidance classification;
        // otherwise propagate it using the non-gracious branch collision path.
        bool gracious = to_bp || p.second == LineStatus::TO_MODEL_GRACIOUS || p.second == LineStatus::TO_MODEL_GRACIOUS_SAFE;
        bool safe_radius = p.second == LineStatus::TO_BP_SAFE || p.second == LineStatus::TO_MODEL_GRACIOUS_SAFE;
        if (! config.support_rests_on_model && ! to_bp) {
            BOOST_LOG_TRIVIAL(warning) << "Tried to add an invalid support point";
            tree_supports_show_error("Unable to add tip. Some overhang may not be supported correctly."sv, true);
            return;
        }
        if (m_pending != nullptr) {
            m_pending->tips.push_back({p, insert_layer, dont_move_until, roof,
                roof_recovery_dtt, skip_ovalisation, roof_already_generated});
            return;
        }
        Polygons circle{ m_base_circle };
        circle.front().translate(p.first);
        {
            Point hash_pos = p.first / ((config.min_radius + 1) / 10);
            std::lock_guard<std::mutex> critical_section_movebounds(m_mutex_movebounds);
            if (!m_already_inserted[insert_layer].count(hash_pos)) {
                // normalize the point a bit to also catch points which are so close that inserting it would achieve nothing
                m_already_inserted[insert_layer].emplace(hash_pos);
                static constexpr const size_t dtt = 0;
                SupportElementState state;
                state.target_height = insert_layer;
                state.target_position = p.first;
                state.next_position = p.first;
                state.layer_idx = insert_layer;
                state.effective_radius_height = dtt;
                state.to_buildplate = to_bp;
                state.distance_to_top = dtt;
                state.result_on_layer = p.first;
                assert(state.result_on_layer_is_set());
                state.increased_to_model_radius = 0;
                state.to_model_gracious = gracious;
                state.elephant_foot_increases = 0;
                state.use_min_xy_dist = min_xy_dist;
                state.supports_roof = roof;
                state.dont_move_until = dont_move_until;
                state.can_use_safe_radius = safe_radius;
                state.set_pending_roof_recovery(force_tip_to_roof && !roof_already_generated ? dont_move_until : 0, roof_recovery_dtt);
                state.skip_ovalisation = skip_ovalisation;
                move_bounds[insert_layer].emplace_back(state, std::move(circle));
            }
        }
    }

    // Outputs
    std::vector<SupportElements>                       &move_bounds;
    PendingPlacement                                  *m_pending;

    // Temps
    coord_t m_base_radius;
    Polygon                                       m_base_circle;

    // Mutexes, guards
    std::mutex                                          m_mutex_movebounds;
    std::vector<std::unordered_set<Point, PointHash>>   m_already_inserted;
};

int generate_raft_contact(
    const PrintObject               &print_object,
    const TreeSupportSettings       &config,
    InterfacePlacer                 &interface_placer)
{
    int raft_contact_layer_idx = -1;
    if (print_object.has_raft() && print_object.layer_count() > 0) {
        // Produce raft contact layer outside of the tree support loop, so that no trees will be generated for the raft contact layer.
        // Raft layers supporting raft contact interface will be produced by the classic raft generator.
        // Find the raft contact layer.
        raft_contact_layer_idx = int(config.raft_layers.size()) - 1;
        while (raft_contact_layer_idx > 0 && config.raft_layers[raft_contact_layer_idx] > print_object.slicing_parameters().raft_contact_top_z + EPSILON)
            -- raft_contact_layer_idx;
        // Create the raft contact layer.
        const ExPolygons &lslices   = print_object.get_layer(0)->lslices;
        double            expansion = print_object.config().raft_expansion.value;
        const ExPolygons raft_contact_areas = build_raft_first_layer_footprint(
            lslices, {}, print_object.config().raft_generate_bounding_box,
            print_object.config().raft_ignore_internal_contours);
        Polygons raft_contact_polygons = expansion > 0 ?
            expand(raft_contact_areas, scaled<float>(expansion)) :
            to_polygons(raft_contact_areas);
        if (!raft_contact_polygons.empty())
            interface_placer.add_roof_unguarded(std::move(raft_contact_polygons), raft_contact_layer_idx, 0);
    }
    return raft_contact_layer_idx;
}

void finalize_raft_contact(
    const PrintObject               &print_object,
    const int                        raft_contact_layer_idx,
    SupportGeneratorLayersPtr       &top_contacts,
    std::vector<SupportElements>    &move_bounds)
{
    if (raft_contact_layer_idx >= 0) {
        const size_t first_tree_layer = print_object.slicing_parameters().raft_layers() - 1;
        // Remove tree tips that start below the raft contact,
        // remove interface layers below the raft contact.
        for (size_t i = 0; i < first_tree_layer; ++i) {
            top_contacts[i] = nullptr;
            move_bounds[i].clear();
        }
        if (raft_contact_layer_idx >= 0 && print_object.config().raft_expansion.value > 0) {
            // If any tips at first_tree_layer now are completely inside the expanded raft layer, remove them as well before they are propagated to the ground.
            Polygons &raft_polygons = top_contacts[raft_contact_layer_idx]->polygons;
            EdgeGrid::Grid grid(get_extents(raft_polygons).inflated(SCALED_EPSILON));
            grid.create(raft_polygons, Polylines{}, coord_t(scale_(10.)));
            SupportElements &first_layer_move_bounds = move_bounds[first_tree_layer];
            double threshold = scaled<double>(print_object.config().raft_expansion.value) * 2.;
            first_layer_move_bounds.erase(std::remove_if(first_layer_move_bounds.begin(), first_layer_move_bounds.end(),
                [&grid, threshold](const SupportElement &el) {
                    coordf_t dist;
                    if (grid.signed_distance_edges(el.state.result_on_layer, threshold, dist)) {
                        assert(std::abs(dist) < threshold + SCALED_EPSILON);
                        // Support point is inside the expanded raft, remove it.
                        return dist < - 0.;
                    }
                    return false;
                }), first_layer_move_bounds.end());
    #if 0
            // Remove the remaining tips from the raft: Closing operation on tip circles.
            if (! first_layer_move_bounds.empty()) {
                const double eps = 0.1;
                // All tips supporting this layer are expected to have the same radius.
                double       radius = support_element_radius(config, first_layer_move_bounds.front());
                // Connect the tips with the following closing radius.
                double       closing_distance = radius;
                Polygon      circle = make_circle(radius + closing_distance, eps);
                Polygons     circles;
                circles.reserve(first_layer_move_bounds.size());
                for (const SupportElement &el : first_layer_move_bounds) {
                    circles.emplace_back(circle);
                    circles.back().translate(el.state.result_on_layer);
                }
                raft_polygons = diff(raft_polygons, offset(union_(circles), - closing_distance));
            }
    #endif
        }
    }
}

// Called by generate_initial_areas(), used in parallel by multiple layers.
// Produce
// 1) Maximum num_support_roof_layers roof (top interface & contact) layers.
// 2) Tree tips supporting either the roof layers or the object itself.
// num_support_roof_layers should always be respected:
// If the requested roof/contact stack cannot be generated directly, the affected tree tips
// carry explicit pending roof recovery metadata so the sliced branch geometry can later be
// promoted back to top contacts / interfaces at the correct contact depth.
void sample_overhang_area(
    // Area to support
    Polygons                           &&overhang_area,
    // Clipped model-side footprint used for contact/interface surfaces.
    // Organic branches continue to use the expanded support area above.
    Polygons                           &&contact_area,
    // Sharp-tail footprint using only the Z-gap collision model.
    Polygons                           &&sharp_tail_contact_area,
    // Sharp convex tip footprint using the configurable local XY/Z values.
    Polygons                           &&tip_contact_area,
    // If true, then the overhang_area is likely large and wide, thus it is worth to try
    // to cover it with continuous interfaces supported by zig-zag patterned tree tips.
    const bool                           large_horizontal_roof,
    // Index of the top suport layer generated by this function.
    const size_t                         layer_idx,
    // Maximum number of roof (contact, interface) layers between the overhang and tree tips to be generated.
    const size_t                         num_support_roof_layers,
    //
    const coord_t                        connect_length,
    // Configuration classes
    const TreeSupportMeshGroupSettings& mesh_group_settings,
    // Configuration & Output
    RichInterfacePlacer& interface_placer)
{
    // Assumption is that roof will support roof further up to avoid a lot of unnecessary branches. Each layer down it is checked whether the roof area
    // is still large enough to be a roof and aborted as soon as it is not. This part was already reworked a few times, and there could be an argument
    // made to change it again if there are actual issues encountered regarding supporting roofs.
    // Main problem is that some patterns change each layer, so just calculating points and checking if they are still valid an layer below is not useful,
    // as the pattern may be different one layer below. Same with calculating which points are now no longer being generated as result from
    // a decreasing roof, as there is no guarantee that a line will be above these points. Implementing a separate roof support behavior
    // for each pattern harms maintainability as it very well could be >100 LOC
    auto generate_roof_lines = [connect_length](const Polygons &area, LayerIndex) -> Polylines {
        return organic_roof_tip_positions(area, connect_length);
    };

    LineInformations        overhang_lines;
    // Keep the polygon that was actually selected for the top contact layer.
    // `overhang_area` is progressively clipped while looking for a lower
    // support layer; after that loop it is no longer the contact footprint.
    // Using that lower-layer remainder for the contact infill produces a
    // necklace of isolated tree-tip caps around the object instead of a
    // continuous surface over the original overhang.
    Polygons                contact_roof_area = std::move(contact_area);
    Polygons                sharp_tail_contact_roof_area = std::move(sharp_tail_contact_area);
    Polygons                tip_contact_roof_area = std::move(tip_contact_area);
    const bool              continuous_contact = !contact_roof_area.empty() || !sharp_tail_contact_roof_area.empty() || !tip_contact_roof_area.empty();
    // Keep the roof polygons pending until the actual tree-tip candidate
    // region has been calculated below.  A continuous contact footprint is
    // only printable when a branch can reach every part of it; committing the
    // full model footprint before sampling tips creates unsupported interface
    // strips at sharp ends.
    std::vector<Polygons> pending_roofs;
    // Track how many top contact / interface layers were already generated.
    size_t                  dtt_roof             = 0;
    size_t                  layer_generation_dtt = 0;

    if (large_horizontal_roof) {
        assert(num_support_roof_layers > 0);
        // Sometimes roofs could be empty as the pattern does not generate lines if the area is narrow enough (i am looking at you, concentric infill).
        // To catch these cases the added roofs are saved to be evaluated later.
        std::vector<Polygons>   added_roofs(num_support_roof_layers);
        Polygons                last_overhang = overhang_area;
        for (dtt_roof = 0; dtt_roof < num_support_roof_layers && layer_idx - dtt_roof >= 1; ++ dtt_roof) {
            // here the roof is handled. If roof can not be added the branches will try to not move instead
            Polygons forbidden_next;
            {
                const bool min_xy_dist = interface_placer.config.xy_distance >= interface_placer.config.xy_min_distance;
                const Polygons &forbidden_next_raw = interface_placer.config.support_rests_on_model ?
                    interface_placer.volumes.getCollision(interface_placer.config.getRadius(0), layer_idx - (dtt_roof + 1), min_xy_dist) :
                    interface_placer.volumes.getAvoidance(interface_placer.config.getRadius(0), layer_idx - (dtt_roof + 1), TreeModelVolumes::AvoidanceType::Fast, false, min_xy_dist);
                // prevent rounding errors down the line
                //FIXME maybe use SafetyOffset::Yes at the following diff() instead?
                forbidden_next = offset(union_ex(forbidden_next_raw), scaled<float>(0.005), jtMiter, 1.2);
            }
            Polygons overhang_area_next = diff(overhang_area, forbidden_next);
            if (area(overhang_area_next) < mesh_group_settings.minimum_roof_area) {
                // Next layer down the roof area would be to small so we have to insert our roof support here.
                if (dtt_roof > 0) {
                    size_t dtt_before = dtt_roof - 1;
                    std::optional<Polygons> contact_collision;
                    // Produce support head points supporting an interface layer: First produce the interface lines, then sample them.
                    overhang_lines = split_lines(
                        convert_lines_to_internal(interface_placer.volumes, interface_placer.config,
                            generate_roof_lines(last_overhang, layer_idx - dtt_before), layer_idx - dtt_before),
                        [&interface_placer, layer_idx, dtt_before, &contact_collision](const std::pair<Point, LineStatus> &p)
                            { return evaluate_point_for_next_layer_function(interface_placer.volumes, interface_placer.config, layer_idx - dtt_before, p, contact_collision); })
                        .first;
                }
                break;
            }
            // Contact and lower interface layers share a continuous footprint,
            // trimmed against the collision volume on each output layer.
            // Branch sampling still uses the original safe support area.
            if (continuous_contact) {
                added_roofs[dtt_roof] = diff(contact_roof_area,
                    interface_placer.volumes.getCollision(0, layer_idx - dtt_roof, interface_placer.min_xy_dist));
                if (!sharp_tail_contact_roof_area.empty())
                    append(added_roofs[dtt_roof], diff(sharp_tail_contact_roof_area,
                        interface_placer.volumes.getCollisionWithoutXY(0, layer_idx - dtt_roof)));
                if (!tip_contact_roof_area.empty())
                    append(added_roofs[dtt_roof], diff(tip_contact_roof_area,
                        interface_placer.volumes.getCollisionForContact(0, layer_idx - dtt_roof,
                            interface_placer.config.support_tip_xy_distance,
                            interface_placer.config.support_tip_z_distance)));
                added_roofs[dtt_roof] = union_(added_roofs[dtt_roof]);
            } else
                added_roofs[dtt_roof] = overhang_area;
            last_overhang   = std::move(overhang_area);
            overhang_area = std::move(overhang_area_next);
        }

        layer_generation_dtt = std::max(dtt_roof, size_t(1)) - 1; // 1 inside max and -1 outside to avoid underflow. layer_generation_dtt=dtt_roof-1 if dtt_roof!=0;
        // If the roof should be valid, check that the area does generate lines.
        // Continuous contact layers are also valid when the narrow
        // overhang does not yield a sampled line: keep the clipped polygon so
        // the contact surface remains continuous instead of falling back to
        // isolated organic tip circles.
        if (overhang_lines.empty() && dtt_roof != 0 &&
            !continuous_contact &&
            generate_roof_lines(overhang_area, layer_idx - layer_generation_dtt).empty())
            for (size_t idx = 0; idx < dtt_roof; idx++) {
                // check for every roof area that it has resulting lines. Remember idx 1 means the 2. layer of roof => higher idx == lower layer
                if (generate_roof_lines(added_roofs[idx], layer_idx - idx).empty()) {
                    dtt_roof = idx;
                    layer_generation_dtt = std::max(dtt_roof, size_t(1)) - 1;
                    break;
                }
            }
        added_roofs.erase(added_roofs.begin() + dtt_roof, added_roofs.end());
        pending_roofs = std::move(added_roofs);
    }

    const bool supports_roof = dtt_roof > 0;
    const bool continuous_tips = !supports_roof && large_horizontal_roof;
    Polygons contact_branch_area;
    if (continuous_contact) {
        // Contact candidates are inserted on the same layer as the sampled
        // overhang lines below. Keeping the collision check on that layer
        // avoids re-evaluating a valid tip against a different model slice.
        const LayerIndex branch_layer = layer_idx - layer_generation_dtt;
        const bool min_xy_dist = interface_placer.config.xy_distance >=
            interface_placer.config.xy_min_distance;
        auto append_contact_branch_area = [&](const Polygons &footprint, Polygons forbidden) {
            if (footprint.empty())
                return;
            // The sampled branch candidates intentionally use the complete
            // collision-clipped contact footprint.  An eroded core used to be
            // calculated and appended before that same full footprint; the
            // core is a subset of the full component, so it could not change
            // the final union and only added expensive offset work.  Keep the
            // original ExPolygon decomposition and per-component difference
            // so the clipping topology and path ordering stay unchanged.
            for (const ExPolygon &component : union_ex(footprint)) {
                Polygons full_component = diff(to_polygons(component), forbidden);
                append(contact_branch_area, std::move(full_component));
            }
        };

        // A continuous contact roof may extend beyond the reduced branch
        // area used for normal tree sampling. Add safe cores for the ordinary
        // contact, sharp-tail exception, and configurable tip exception
        // separately so each keeps its own collision rule.
        append_contact_branch_area(
            contact_roof_area,
            interface_placer.volumes.getAvoidance(
                interface_placer.config.getRadius(0), branch_layer,
                TreeModelVolumes::AvoidanceType::Fast, false, min_xy_dist));
        append_contact_branch_area(
            sharp_tail_contact_roof_area,
            interface_placer.volumes.getCollisionWithoutXY(
                interface_placer.config.getRadius(0), branch_layer));
        append_contact_branch_area(
            tip_contact_roof_area,
            interface_placer.volumes.getCollisionForContact(
                interface_placer.config.getRadius(0), branch_layer,
                interface_placer.config.support_tip_xy_distance,
                interface_placer.config.support_tip_z_distance));
        if (!contact_branch_area.empty())
            contact_branch_area = union_(std::move(contact_branch_area));

        // Keep the full model-side contact footprint. The branch candidates
        // below are generated from that same footprint (and collision filtered
        // independently), so a narrow tree core cannot make the requested
        // contact fill disappear.
    }

    // Roofs without a separate contact footprint still need to be committed;
    // delay only changes the ordering, not the ordinary non-contact path.
    if (!pending_roofs.empty())
        interface_placer.add_roofs(std::move(pending_roofs), layer_idx);

    // Even when the roof sampler already produced lines, add safe tree-tip
    // candidates for the contact-only portion. Previously this was done only
    // in the empty-line fallback, so a continuous interface could be emitted
    // with no organic tree points underneath its outer edge.
    auto uncovered_contact_area = [&overhang_lines, &interface_placer, &contact_branch_area]() {
        Polygons covered_by_tips;
        for (const LineInformation &line : overhang_lines)
            for (const auto &[point, status] : line)
                if (status != LineStatus::INVALID) {
                    Polygon footprint = make_circle(interface_placer.config.min_radius, SUPPORT_TREE_CIRCLE_RESOLUTION);
                    footprint.translate(point);
                    covered_by_tips.push_back(std::move(footprint));
                }
        return covered_by_tips.empty() ? contact_branch_area : diff(contact_branch_area, union_(covered_by_tips));
    };
    if (!overhang_lines.empty() && !contact_branch_area.empty()) {
        const LayerIndex branch_layer = layer_idx - layer_generation_dtt;
        const Polygons uncovered = uncovered_contact_area();
        Polylines contact_polylines = continuous_contact ? organic_roof_tip_positions(uncovered, connect_length) :
            ensure_maximum_distance_polyline(generate_support_infill_lines(uncovered,
                interface_placer.support_parameters, false, branch_layer, mesh_group_settings.support_tree_branch_distance),
                continuous_tips ? interface_placer.config.min_radius / 2 : connect_length, 1);
        append(overhang_lines, convert_contact_lines_to_internal(
            interface_placer.volumes,
            interface_placer.config,
            contact_polylines,
            branch_layer));
    }

    if (overhang_lines.empty()) {
        // support_line_width to form a line here as otherwise most will be unsupported. Technically this violates branch distance, but not only is this the only reasonable choice,
        // but it ensures consistant behaviour as some infill patterns generate each line segment as its own polyline part causing a similar line forming behaviour.
        // This is not doen when a roof is above as the roof will support the model and the trees only need to support the roof
        Polylines polylines = continuous_contact ? organic_roof_tip_positions(overhang_area, connect_length) :
            ensure_maximum_distance_polyline(
                generate_support_infill_lines(overhang_area, interface_placer.support_parameters, false, layer_idx - layer_generation_dtt,
                    mesh_group_settings.support_tree_branch_distance),
                continuous_tips ? interface_placer.config.min_radius / 2 : connect_length, 1);
        size_t point_count = 0;
        for (const Polyline &poly : polylines)
            point_count += poly.size();
        const size_t min_support_points = std::max(coord_t(1), std::min(coord_t(3), coord_t(total_length(overhang_area) / connect_length)));
        if (point_count <= min_support_points) {
            // add the outer wall (of the overhang) to ensure it is correct supported instead. Try placing the support points in a way that they fully support the outer wall, instead of just the with half of the support line width.
            // I assume that even small overhangs are over one line width wide, so lets try to place the support points in a way that the full support area generated from them
            // will support the overhang (if this is not done it may only be half). This WILL NOT be the case when supporting an angle of about < 60 degrees so there is a fallback,
            // as some support is better than none.
            Polygons reduced_overhang_area = offset(union_ex(overhang_area), -interface_placer.config.support_line_width / 2.2, jtMiter, 1.2);
            polylines = ensure_maximum_distance_polyline(
                to_polylines(
                    ! reduced_overhang_area.empty() &&
                        area(offset(diff_ex(overhang_area, reduced_overhang_area), std::max(interface_placer.config.support_line_width, connect_length), jtMiter, 1.2)) < sqr(scaled<double>(0.001)) ?
                    reduced_overhang_area :
                    overhang_area),
                connect_length, min_support_points);
        }
        overhang_lines = convert_lines_to_internal(interface_placer.volumes, interface_placer.config, polylines, layer_idx - dtt_roof);
        // Contact-only candidates must not be reclassified with the ordinary
        // XY clearance.  This path is used when the roof sampler produced no
        // normal lines, which is common for narrow one-sided components.
        if (!contact_branch_area.empty()) {
            const Polygons uncovered = uncovered_contact_area();
            Polylines contact_polylines = continuous_contact ? organic_roof_tip_positions(uncovered, connect_length) :
                ensure_maximum_distance_polyline(generate_support_infill_lines(uncovered,
                    interface_placer.support_parameters, false, layer_idx - layer_generation_dtt,
                    mesh_group_settings.support_tree_branch_distance),
                    continuous_tips ? interface_placer.config.min_radius / 2 : connect_length, 1);
            append(overhang_lines, convert_contact_lines_to_internal(
                interface_placer.volumes,
                interface_placer.config,
                contact_polylines,
                layer_idx - layer_generation_dtt));
        }
    }

    assert(dtt_roof <= layer_idx);
    if (dtt_roof >= layer_idx && large_horizontal_roof) {
        // Reached the build plate while generating the roof stack.  The
        // support-layer remainder is only the area where a tree branch can
        // stand; it is not the footprint of the model layer that this roof
        // must support.  In particular, layer 0 is the layer immediately
        // below the first overhanging model layer.  A continuous contact
        // must therefore keep the footprint captured from that next model
        // layer instead of the progressively clipped support remainder.
        // Keep the contact footprint derived from the next model layer and
        // apply only the collision rule for the actual output layer.
        const LayerIndex output_layer_idx = layer_idx - dtt_roof;
        Polygons build_plate_roof;
        if (continuous_contact) {
            build_plate_roof = diff(contact_roof_area,
                interface_placer.volumes.getCollision(0, output_layer_idx, interface_placer.min_xy_dist));
            if (!sharp_tail_contact_roof_area.empty())
                append(build_plate_roof, diff(sharp_tail_contact_roof_area,
                    interface_placer.volumes.getCollisionWithoutXY(0, output_layer_idx)));
            if (!tip_contact_roof_area.empty())
                append(build_plate_roof, diff(tip_contact_roof_area,
                    interface_placer.volumes.getCollisionForContact(0, output_layer_idx,
                        interface_placer.config.support_tip_xy_distance,
                        interface_placer.config.support_tip_z_distance)));
            build_plate_roof = build_plate_roof.empty() ? Polygons{} : union_(std::move(build_plate_roof));
        } else {
            build_plate_roof = std::move(overhang_area);
        }
        // Reached buildplate when generating contact, interface and base
        // interface layers.
        interface_placer.add_roof_build_plate(std::move(build_plate_roof), dtt_roof);
    }
    else {
        // normal trees have to be generated
        const bool roof_enabled = num_support_roof_layers > 0;
        // The tip mask uses the same roof stack, but its collision footprint is
        // evaluated with the local XY/Z clearances. Keep this path separate
        // from ordinary contacts so the exception cannot widen all branches.
        interface_placer.add_points_along_lines(
            // Sample along these lines
            overhang_lines,
            // First layer index to insert the tree tips or interfaces.
            layer_idx - dtt_roof,
            // Remaining roof tip layers.
            interface_placer.force_tip_to_roof ? num_support_roof_layers - dtt_roof : 0,
            // Supports roof already? How many roof layers were already produced above these tips?
            dtt_roof,
            // Don't move until the following distance to top is reached.
            roof_enabled ? num_support_roof_layers - dtt_roof : 0,
            contact_roof_area.empty() ? nullptr : &contact_roof_area,
            sharp_tail_contact_roof_area.empty() ? nullptr : &sharp_tail_contact_roof_area,
            tip_contact_roof_area.empty() ? nullptr : &tip_contact_roof_area);
    }
}

/*!
 * \brief Creates the initial influence areas (that can later be propagated down) by placing them below the overhang.
 *
 * Generates Points where the Model should be supported and creates the areas where these points have to be placed.
 *
 * \param mesh[in] The mesh that is currently processed.
 * \param move_bounds[out] Storage for the influence areas.
 * \param storage[in] Background storage, required for adding roofs.
 */
static void generate_initial_areas(
    const PrintObject               &print_object,
    const TreeModelVolumes          &volumes,
    const TreeSupportSettings       &config,
    const std::vector<Polygons>     &overhangs,
    const std::vector<Polygons>     &sharp_tail_overhangs,
    std::vector<SupportElements>    &move_bounds,
    InterfacePlacer                 &interface_placer,
    std::function<void()>            throw_on_cancel)
{
    using                           AvoidanceType = TreeModelVolumes::AvoidanceType;
    TreeSupportMeshGroupSettings    mesh_group_settings(print_object);

    // To ensure z_distance_top_layers are left empty between the overhang (zeroth empty layer), the support has to be added z_distance_top_layers+1 layers below
    // Keep the source-layer mapping aligned with the object layer immediately
    // above the contact surface when the configured Z gap is zero.
    const size_t z_distance_delta = config.z_distance_top_layers + 1;

    const bool min_xy_dist = config.xy_distance >= config.xy_min_distance;

#if 0
    if (mesh.overhang_areas.size() <= z_distance_delta)
        return;
#endif

    const coord_t connect_length = (config.support_line_width * 100. / mesh_group_settings.support_tree_top_rate) + std::max(2. * config.min_radius - 1.0 * config.support_line_width, 0.0);
    // As r*r=x*x+y*y (circle equation): If a circle with center at (0,0) the top most point is at (0,r) as in y=r.
    // This calculates how far one has to move on the x-axis so that y=r-support_line_width/2.
    // In other words how far does one need to move on the x-axis to be support_line_width/2 away from the circle line.
    // As a circle is round this length is identical for every axis as long as the 90 degrees angle between both remains.
    const coord_t circle_length_to_half_linewidth_change = config.min_radius < config.support_line_width ?
        config.min_radius / 2 :
        scale_(sqrt(sqr(unscale<double>(config.min_radius)) - sqr(unscale<double>(config.min_radius - config.support_line_width / 2))));
    // Extra support offset to compensate for larger tip radiis. Also outset a bit more when z overwrites xy, because supporting something with a part of a support line is better than not supporting it at all.
    //FIXME Vojtech: This is not sufficient for support enforcers to work.
    //FIXME There is no account for the support overhang angle.
    //FIXME There is no account for the width of the collision regions.
    const coord_t extra_outset = std::max(coord_t(0), config.min_radius - config.support_line_width / 2) + (min_xy_dist ? config.support_line_width / 2 : 0)
        //FIXME this is a heuristic value for support enforcers to work.
//        + 10 * config.support_line_width;
        ;
    const size_t  num_support_roof_layers = mesh_group_settings.support_roof_layers;
    const bool    roof_enabled        = num_support_roof_layers > 0;
    // Force a small tree tip into a roof only when the contact is zero-gap or
    // when the tip itself is large enough to form a valid roof footprint.
    const bool    force_tip_to_roof   = roof_enabled &&
        (interface_placer.support_parameters.zero_gap_interface_top ||
         sqr<double>(config.min_radius) * M_PI > mesh_group_settings.minimum_roof_area);
    // cap for how much layer below the overhang a new support point may be added, as other than with regular support every new inserted point
    // may cause extra material and time cost.  Could also be an user setting or differently calculated. Idea is that if an overhang
    // does not turn valid in double the amount of layers a slope of support angle would take to travel xy_distance, nothing reasonable will come from it.
    // The 2*z_distance_delta is only a catch for when the support angle is very high.
    // Used only if not min_xy_dist.
    coord_t max_overhang_insert_lag = 0;
    if (config.z_distance_top_layers > 0) {
        max_overhang_insert_lag = 2 * config.z_distance_top_layers;

    //FIXME
        if (mesh_group_settings.support_angle > EPSILON && mesh_group_settings.support_angle < 0.5 * M_PI - EPSILON) {
            //FIXME mesh_group_settings.support_angle does not apply to enforcers and also it does not apply to automatic support angle (by half the external perimeter width).
            //used by max_overhang_insert_lag, only if not min_xy_dist.
            const auto max_overhang_speed  = coord_t(tan(mesh_group_settings.support_angle) * config.layer_height);
            max_overhang_insert_lag = std::max(max_overhang_insert_lag, round_up_divide(config.xy_distance, max_overhang_speed / 2));
        }
    }

    size_t                                          num_support_layers;
    int                                             raft_contact_layer_idx;
    // Layers with their overhang regions.
    struct RawOverhang {
        size_t layer_idx;
        const Polygons *areas;
        const Polygons *sharp_tail_areas;
    };
    std::vector<RawOverhang> raw_overhangs;

    {
        const size_t num_raft_layers     = config.raft_layers.size();
        const size_t first_support_layer = std::max(int(num_raft_layers) - int(z_distance_delta), roof_enabled ? 0 : 1);
        num_support_layers  = size_t(std::max(0, int(print_object.layer_count()) + int(num_raft_layers) - int(z_distance_delta)));
        raft_contact_layer_idx = generate_raft_contact(print_object, config, interface_placer);
        // Enumerate layers for which the support tips may be generated from overhangs above.
        raw_overhangs.reserve(num_support_layers - first_support_layer);
        for (size_t layer_idx = first_support_layer; layer_idx < num_support_layers; ++ layer_idx)
            if (const size_t overhang_idx = layer_idx + z_distance_delta; ! overhangs[overhang_idx].empty())
                raw_overhangs.push_back({ layer_idx, &overhangs[overhang_idx],
                    overhang_idx < sharp_tail_overhangs.size() && !sharp_tail_overhangs[overhang_idx].empty() ?
                        &sharp_tail_overhangs[overhang_idx] : nullptr });
    }

    trace_organic_timing_progress("initial.raw_overhangs", 0, raw_overhangs.size());

    std::vector<RichInterfacePlacer::PendingPlacement> placements(raw_overhangs.size());
    std::atomic<size_t> completed_raw_overhangs{0};

    tbb::parallel_for(tbb::blocked_range<size_t>(0, raw_overhangs.size()),
        [&volumes, &config, &print_object, &raw_overhangs, &mesh_group_settings, &interface_placer, &move_bounds, &placements,
         &completed_raw_overhangs,
         z_distance_delta,
         min_xy_dist, roof_enabled, num_support_roof_layers, extra_outset, circle_length_to_half_linewidth_change, connect_length,
         force_tip_to_roof, num_support_layers, &throw_on_cancel](const tbb::blocked_range<size_t> &range) {
        for (size_t raw_overhang_idx = range.begin(); raw_overhang_idx < range.end(); ++ raw_overhang_idx) {
            RichInterfacePlacer rich_interface_placer{ interface_placer, volumes, force_tip_to_roof,
                num_support_layers, move_bounds, &placements[raw_overhang_idx] };
            size_t           layer_idx    = raw_overhangs[raw_overhang_idx].layer_idx;
            const Polygons  &overhang_raw = *raw_overhangs[raw_overhang_idx].areas;
            const Polygons  *sharp_tail_raw = raw_overhangs[raw_overhang_idx].sharp_tail_areas;
            Polygons contact_roof;
            Polygons sharp_tail_contact_roof;
            Polygons tip_contact_roof;
            if (roof_enabled) {
                // Contact infill needs only the extrusion footprint to clear
                // the model, not the full tree-tip radius. Use the same
                // polygon offset/trim operation as ordinary support contacts.
                // Expand by one roof line width so the contact bead's
                // centreline reaches the model-facing edge after the
                // overhang difference is clipped.  Collision subtraction
                // below still prevents overlap; this compensates the
                // half-line-width inset that otherwise leaves a one-sided
                // outer-ring gap.
                const coord_t contact_margin = std::min<coord_t>(
                    config.min_radius, mesh_group_settings.support_roof_line_width);
                contact_roof = diff(
                    offset(union_ex(overhang_raw), contact_margin, jtRound, SUPPORT_TREE_CIRCLE_RESOLUTION),
                    volumes.getCollision(0, layer_idx, true));
                if (sharp_tail_raw != nullptr) {
                    // Sharp tails are the one intentional exception to the
                    // normal XY clearance rule.  Keep the tree itself away
                    // from the model, but let the dense contact footprint
                    // follow the model using only the configured Z gap.
                    sharp_tail_contact_roof = diff(
                        offset(union_ex(*sharp_tail_raw), contact_margin, jtRound, SUPPORT_TREE_CIRCLE_RESOLUTION),
                        volumes.getCollisionWithoutXY(0, layer_idx));
                }

                // Apply the reduced clearances only around qualifying convex
                // tips. The ordinary support area and branch collision rules
                // remain unchanged, so broad overhangs do not inherit the
                // local relaxation.
                const coord_t tip_mask_radius = std::max(config.min_radius, mesh_group_settings.support_roof_line_width / 2);
                const size_t object_layer_idx = layer_idx + z_distance_delta - config.raft_layers.size();
                // Classify the model contour, not the thin difference strip
                // left by overhang detection. Otherwise almost every sloping
                // face looks like a sharp edge and real model tips are lost.
                Polygons model_tip_mask;
                if (object_layer_idx < print_object.layer_count())
                    model_tip_mask = sharp_tip_contact_mask(
                        to_polygons(print_object.get_layer(object_layer_idx)->lslices),
                        config.support_tip_turn_angle, tip_mask_radius,
                        &overhang_raw);
                Polygons tip_mask = std::move(model_tip_mask);
                // Thin edge/bed intersections may have no qualifying corner
                // in the model slice. Keep their contact coverage; duplicate
                // tree heads are handled by the sampler, not by deleting the
                // surface that the contact is supposed to support.
                append(tip_mask, sharp_tip_contact_mask(overhang_raw,
                    config.support_tip_turn_angle, tip_mask_radius));
                if (!tip_mask.empty()) {
                    tip_contact_roof = diff(std::move(tip_mask),
                        volumes.getCollisionForContact(0, layer_idx,
                            config.support_tip_xy_distance,
                            config.support_tip_z_distance));
                }
            }

            // Keep each support component paired with the part of the model
            // footprint it can actually reach.  Sharing the complete layer
            // footprint with every component lets a one-sided tip emit a
            // short contact arc on the opposite side of the overhang.
            auto contact_for_support = [&](const Polygons &support_area,
                                           const Polygons &footprint,
                                           bool tip_platform = false,
                                           const Polygons *shared_support_reach = nullptr,
                                           bool ordinary_contact = true,
                                           const Polygons *all_support_union = nullptr) {
                if (support_area.empty())
                    return Polygons{};
                // The organic branch area is already the collision-safe
                // `overhang_roofs` footprint.  Keep that complete footprint
                // as the contact platform; restricting it to an individual
                // branch's reach breaks a broad face into small islands even
                // though the branches jointly support the same roof.
                // The support area describes where tree tips may stand; it
                // is not itself an instruction to print a second outer
                // contact loop. Keep ordinary contact inside the model-side
                // footprint whenever one is available. This prevents a
                // tree-tip safety ring from becoming an unsupported skirt.
                Polygons contact = ordinary_contact && footprint.empty() ?
                    support_area : Polygons{};
                if (footprint.empty())
                    return contact;
                // The wide sharp-tip exception is reserved for the default
                // 45-degree mask.  At the user's 90-degree threshold a tip
                // is a broad corner/edge contact and its incident bridge must
                // stay local to 0.5 mm instead of inheriting the shared
                // four-radius reach envelope.
                const bool local_tip_platform = tip_platform &&
                    config.support_tip_turn_angle <= 0.5 * M_PI + EPSILON;
                const coord_t tip_bridge_reach = std::min<coord_t>(
                    4 * config.min_radius, scaled<coord_t>(0.5));
                const coord_t reach = config.min_radius + mesh_group_settings.support_roof_line_width +
                    (local_tip_platform ? tip_bridge_reach : (tip_platform ? 4 * config.min_radius : 0));
                Polygons support_reach;
                if (shared_support_reach != nullptr && !shared_support_reach->empty()) {
                    // Tip contacts and their branch candidates must use the
                    // same union reach envelope on this layer.  Previously a
                    // local tip fell back to its individual component unless
                    // a second *independent* support component happened to
                    // overlap the footprint.  A one-sided overhang therefore
                    // lost its opposite half even though the union envelope
                    // already represented the tree candidates that could
                    // carry it.  The tip footprint itself is still capped by
                    // sharp_tip_contact_mask() (the incident V is at most
                    // 0.5 mm), so this does not widen ordinary contacts or
                    // create a long one-sided V.
                    support_reach = *shared_support_reach;
                } else {
                    support_reach = offset(union_ex(support_area), reach,
                        jtRound, SUPPORT_TREE_CIRCLE_RESOLUTION);
                }
                Polygons clipped = intersection(footprint, support_reach);
                // `support_area` is the collision-safe center region for
                // branch placement and is intentionally eroded from the
                // model.  It is therefore too restrictive for the printed
                // contact footprint: using support_area ∩ footprint here
                // leaves a visible gap at an outer-ring start even though a
                // branch can safely reach the region.  The shared reach
                // envelope already limits the contact to an area covered by
                // the support union; use that envelope for ordinary contact
                // paths as well.
                if (ordinary_contact)
                    contact = clipped;
                // A sharp-tip platform is an explicit local exception: when
                // both incident sides are being supported, keep the complete
                // collision-clipped tip footprint so the contact surface is
                // continuous instead of two disconnected arcs. Long narrow
                // overhangs are already capped to a 0.5 mm contour wrap in
                // sharp_tip_contact_mask(), so this does not restore the old
                // oversized outer loop for the user's blade-like case.
                if (tip_platform && !local_tip_platform)
                    clipped = footprint;
                if (!clipped.empty())
                    contact = union_(std::move(contact), std::move(clipped));
                return contact;
            };

            // take the least restrictive avoidance possible
            Polygons relevant_forbidden;
            {
                const Polygons &relevant_forbidden_raw = config.support_rests_on_model ?
                    volumes.getCollision(config.getRadius(0), layer_idx, min_xy_dist) :
                    volumes.getAvoidance(config.getRadius(0), layer_idx, AvoidanceType::Fast, false, min_xy_dist);
                // prevent rounding errors down the line, points placed directly on the line of the forbidden area may not be added otherwise.
                relevant_forbidden = offset(union_ex(relevant_forbidden_raw), scaled<float>(0.005), jtMiter, 1.2);
            }

            // every overhang has saved if a roof should be generated for it. This can NOT be done in the for loop as an area may NOT have a roof
            // even if it is larger than the minimum_roof_area when it is only larger because of the support horizontal expansion and
            // it would not have a roof if the overhang is offset by support roof horizontal expansion instead. (At least this is the current behavior of the regular support)
            Polygons overhang_regular;
            {
                // When support_offset = 0 safe_offset_inc will only be the difference between overhang_raw and relevant_forbidden, that has to be calculated anyway.
                overhang_regular = safe_offset_inc(overhang_raw, mesh_group_settings.support_offset, relevant_forbidden, config.min_radius * 1.75 + config.xy_min_distance, 0, 1);
                //check_self_intersections(overhang_regular, "overhang_regular1");

                // offset ensures that areas that could be supported by a part of a support line, are not considered unsupported overhang
                Polygons remaining_overhang = intersection(
                    diff(mesh_group_settings.support_offset == 0 ?
                            overhang_raw :
                            offset(union_ex(overhang_raw), mesh_group_settings.support_offset, jtMiter, 1.2),
                         offset(union_ex(overhang_regular), config.support_line_width * 0.5, jtMiter, 1.2)),
                    relevant_forbidden);

                // Offset the area to compensate for large tip radiis. Offset happens in multiple steps to ensure the tip is as close to the original overhang as possible.
                //+config.support_line_width / 80  to avoid calculating very small (useless) offsets because of rounding errors.
                //FIXME likely a better approach would be to find correspondences between the full overhang and the trimmed overhang
                // and if there is no correspondence, project the missing points to the clipping curve.
                for (coord_t extra_total_offset_acc = 0; ! remaining_overhang.empty() && extra_total_offset_acc + config.support_line_width / 8 < extra_outset; ) {
                    const coord_t offset_current_step = std::min(
                        extra_total_offset_acc + 2 * config.support_line_width > config.min_radius ?
                            config.support_line_width / 8 :
                            circle_length_to_half_linewidth_change,
                        extra_outset - extra_total_offset_acc);
                    extra_total_offset_acc += offset_current_step;
                    const Polygons &raw_collision = volumes.getCollision(0, layer_idx, true);
                    const coord_t   offset_step   = config.xy_min_distance + config.support_line_width;
                    // Reducing the remaining overhang by the areas already supported.
                    //FIXME 1.5 * extra_total_offset_acc seems to be too much, it may remove some remaining overhang without being supported at all.
                    remaining_overhang = diff(remaining_overhang, safe_offset_inc(overhang_regular, 1.5 * extra_total_offset_acc, raw_collision, offset_step, 0, 1));
                    // Extending the overhangs by the inflated remaining overhangs.
                    overhang_regular   = union_(overhang_regular, diff(safe_offset_inc(remaining_overhang, extra_total_offset_acc, raw_collision, offset_step, 0, 1), relevant_forbidden));
                    //check_self_intersections(overhang_regular, "overhang_regular2");
                }
#if 0
                // If the xy distance overrides the z distance, some support needs to be inserted further down.
                //=> Analyze which support points do not fit on this layer and check if they will fit a few layers down (while adding them an infinite amount of layers down would technically be closer the setting description, it would not produce reasonable results. )
                if (! min_xy_dist) {
                    LineInformations overhang_lines;
                    {
                        //Vojtech: Generate support heads at support_tree_branch_distance spacing by producing a zig-zag infill at support_tree_branch_distance spacing,
                        // which is then resmapled
                        // support_line_width to form a line here as otherwise most will be unsupported. Technically this violates branch distance,
                        // mbut not only is this the only reasonable choice, but it ensures consistent behavior as some infill patterns generate
                        // each line segment as its own polyline part causing a similar line forming behavior. Also it is assumed that
                        // the area that is valid a layer below is to small for support roof.
                        Polylines polylines = ensure_maximum_distance_polyline(
                            generate_support_infill_lines(remaining_overhang, support_params, false, layer_idx, mesh_group_settings.support_tree_branch_distance),
                            config.min_radius, 1);
                        if (polylines.size() <= 3)
                            // add the outer wall to ensure it is correct supported instead
                            polylines = ensure_maximum_distance_polyline(to_polylines(remaining_overhang), connect_length, 3);
                        for (const auto &line : polylines) {
                            LineInformation res_line;
                            for (Point p : line)
                                res_line.emplace_back(p, LineStatus::INVALID);
                            overhang_lines.emplace_back(res_line);
                        }
                        validate_range(overhang_lines);
                    }
                    for (size_t lag_ctr = 1; lag_ctr <= max_overhang_insert_lag && !overhang_lines.empty() && layer_idx - coord_t(lag_ctr) >= 1; lag_ctr++) {
                        // get least restricted avoidance for layer_idx-lag_ctr
                        const Polygons &relevant_forbidden_below = config.support_rests_on_model ?
                            volumes.getCollision(config.getRadius(0), layer_idx - lag_ctr, min_xy_dist) :
                            volumes.getAvoidance(config.getRadius(0), layer_idx - lag_ctr, AvoidanceType::Fast, false, min_xy_dist);
                        // it is not required to offset the forbidden area here as the points wont change: If points here are not inside the forbidden area neither will they be later when placing these points, as these are the same points.
                        auto evaluatePoint = [&](std::pair<Point, LineStatus> p) { return contains(relevant_forbidden_below, p.first); };

                        std::pair<LineInformations, LineInformations> split = split_lines(overhang_lines, evaluatePoint); // keep all lines that are invalid
                        overhang_lines = split.first;
                        // Set all now valid lines to their correct LineStatus. Easiest way is to just discard Avoidance information for each point and evaluate them again.
                        LineInformations fresh_valid_points = convert_lines_to_internal(volumes, config, convert_internal_to_lines(split.second), layer_idx - lag_ctr);
                        validate_range(fresh_valid_points);

                        rich_interface_placer.add_points_along_lines(fresh_valid_points, (force_tip_to_roof && lag_ctr <= num_support_roof_layers) ? num_support_roof_layers : 0, layer_idx - lag_ctr, false, roof_enabled ? num_support_roof_layers : 0);
                    }
                }
#endif
            }

            throw_on_cancel();

            if (roof_enabled) {
                // Try to support the overhangs by dense interfaces for num_support_roof_layers, cover the bottom most interface with tree tips.
                static constexpr const coord_t support_roof_offset = 0;
                Polygons overhang_roofs = safe_offset_inc(overhang_raw, support_roof_offset, relevant_forbidden, config.min_radius * 2 + config.xy_min_distance, 0, 1);
                if (mesh_group_settings.minimum_support_area > 0)
                    remove_small(overhang_roofs, mesh_group_settings.minimum_roof_area);
                overhang_regular = diff(overhang_regular, overhang_roofs, ApplySafetyOffset::Yes);
                //check_self_intersections(overhang_regular, "overhang_regular3");
                // A sharp-tip contact footprint can be printable even when
                // the normal overhang difference retained only one incident
                // side.  Keep that local, collision-clipped footprint in the
                // tree support seed as well; otherwise the roof can be emitted
                // on one side while no organic branch is ever allowed to
                // reach the other side.  The footprint is already bounded by
                // sharp_tip_contact_mask() and the local XY/Z collision rule,
                // so this does not widen ordinary support regions.
                Polygons roof_support_seed = overhang_roofs;
                // The tip footprint has already been clipped with the
                // configured local XY/Z contact clearance.  It therefore
                // must also seed the organic branch planner for every tip
                // angle.  Restricting this to the old <=90-degree branch
                // left a valid contact roof without a tree below it for the
                // normal 45-degree setting, especially once the ordinary
                // support XY clearance exceeded the narrow edge width.
                if (!tip_contact_roof.empty())
                    append(roof_support_seed, tip_contact_roof);
                roof_support_seed = roof_support_seed.empty() ? Polygons{} : union_(std::move(roof_support_seed));
                std::vector<Polygons> roof_support_areas;
                for (ExPolygon &roof_part : union_ex(roof_support_seed))
                    roof_support_areas.emplace_back(to_polygons(std::move(roof_part)));
                Polygons roof_support_union;
                for (const Polygons &support_area : roof_support_areas)
                    append(roof_support_union, support_area);
                roof_support_union = roof_support_union.empty() ? Polygons{} : union_(roof_support_union);
                const coord_t roof_contact_reach = config.min_radius + mesh_group_settings.support_roof_line_width;
                // Keep the shared reach envelope for two-sided tip
                // connections; the emitted contact footprint is still
                // clipped to that envelope rather than copied wholesale.
                const coord_t roof_tip_reach = roof_contact_reach + 4 * config.min_radius;
                const Polygons roof_shared_reach = roof_support_union.empty() ? Polygons{} :
                    offset(union_ex(roof_support_union), roof_contact_reach, jtRound, SUPPORT_TREE_CIRCLE_RESOLUTION);
                const Polygons roof_shared_tip_reach = roof_support_union.empty() ? Polygons{} :
                    offset(union_ex(roof_support_union), roof_tip_reach, jtRound, SUPPORT_TREE_CIRCLE_RESOLUTION);
                for (Polygons &support_area : roof_support_areas) {
                    // The safe tree area is deliberately offset away from the
                    // model, so it normally has no geometric intersection with
                    // the original overhang.  Keep the original footprint as
                    // the contact seed.  A shared reach envelope joins
                    // neighboring components before the interface polygons
                    // are accumulated, so the platform is not cut into strips.
                    // A sharp-tip footprint is the only region allowed to
                    // use the reduced local contact rule. Remove it from the
                    // ordinary footprint first; otherwise the normal contact
                    // path recreates the long V/necklace around a tip even
                    // though the tip-specific mask was capped to 0.5 mm.
                    Polygons ordinary_contact_roof = contact_roof;
                    if (!tip_contact_roof.empty())
                        ordinary_contact_roof = diff(ordinary_contact_roof, tip_contact_roof);
                    Polygons contact_area = contact_for_support(support_area, ordinary_contact_roof, false, &roof_shared_reach, true, &roof_support_union);
                    Polygons sharp_tail_contact_area = contact_for_support(support_area, sharp_tail_contact_roof, false, &roof_shared_reach, false, &roof_support_union);
                    Polygons tip_contact_area = contact_for_support(support_area, tip_contact_roof, true, &roof_shared_tip_reach, false, &roof_support_union);
                    sample_overhang_area(std::move(support_area), std::move(contact_area), std::move(sharp_tail_contact_area), std::move(tip_contact_area), true, layer_idx, num_support_roof_layers, connect_length,
                        mesh_group_settings, rich_interface_placer);
                    throw_on_cancel();
                }
            }
            // Either the roof is not enabled, then these are all the overhangs to be supported,
            // or roof is enabled and these are the thin overhangs at object slopes (not horizontal overhangs).
            if (mesh_group_settings.minimum_support_area > 0)
                remove_small(overhang_regular, mesh_group_settings.minimum_support_area);

            std::vector<Polygons> regular_support_areas;
            for (ExPolygon &support_part : union_ex(overhang_regular))
                regular_support_areas.emplace_back(to_polygons(std::move(support_part)));
            Polygons regular_support_union;
            for (const Polygons &support_area : regular_support_areas)
                append(regular_support_union, support_area);
            regular_support_union = regular_support_union.empty() ? Polygons{} : union_(regular_support_union);
            const coord_t regular_contact_reach = config.min_radius + mesh_group_settings.support_roof_line_width;
            const coord_t regular_tip_reach = regular_contact_reach + 4 * config.min_radius;
            const Polygons regular_shared_reach = regular_support_union.empty() ? Polygons{} :
                offset(union_ex(regular_support_union), regular_contact_reach, jtRound, SUPPORT_TREE_CIRCLE_RESOLUTION);
            const Polygons regular_shared_tip_reach = regular_support_union.empty() ? Polygons{} :
                offset(union_ex(regular_support_union), regular_tip_reach, jtRound, SUPPORT_TREE_CIRCLE_RESOLUTION);
            for (Polygons &support_area : regular_support_areas) {
                // `overhang_regular` contains the thin/sloped remainder after
                // actual roof regions have been removed. It still needs a
                // contact footprint for narrow tips, but that footprint must
                // stay local to this tree component. Using the shared union
                // envelope here lets a distant component promote an
                // otherwise unsupported wall strip into an interface.
                Polygons ordinary_contact_roof = contact_roof;
                if (!tip_contact_roof.empty())
                    ordinary_contact_roof = diff(ordinary_contact_roof, tip_contact_roof);
                Polygons contact_area = contact_for_support(support_area, ordinary_contact_roof, false, nullptr, true, nullptr);
                Polygons sharp_tail_contact_area = contact_for_support(support_area, sharp_tail_contact_roof, false, &regular_shared_reach, false, &regular_support_union);
                Polygons tip_contact_area = contact_for_support(support_area, tip_contact_roof, true, &regular_shared_tip_reach, false, &regular_support_union);
                sample_overhang_area(std::move(support_area), std::move(contact_area), std::move(sharp_tail_contact_area), std::move(tip_contact_area),
                    false, layer_idx, num_support_roof_layers, connect_length,
                    mesh_group_settings, rich_interface_placer);
                throw_on_cancel();
            }
            const size_t completed = completed_raw_overhangs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (completed == 1 || completed == raw_overhangs.size() || completed % 100 == 0)
                trace_organic_timing_progress("initial.raw_overhangs", completed, raw_overhangs.size());
        }
    });

    trace_organic_timing("initial.raw_overhangs.complete");

    // A tip can descend onto a layer also targeted by another overhang. A mutex
    // only prevented races; the first arriving candidate still changed which
    // point survived quantized deduplication, and hence the entire tree. Keep
    // the expensive clipping/sampling parallel, but commit those outputs in
    // the same order as a single-worker run, including the roof polygon order.
    RichInterfacePlacer rich_interface_placer{ interface_placer, volumes, force_tip_to_roof, num_support_layers, move_bounds };
    for (RichInterfacePlacer::PendingPlacement &placement : placements)
        rich_interface_placer.publish_roofs(placement);

    // Expanded sampling areas are only candidates. Reject roofs without model
    // demand before publishing their tips, otherwise a discarded contact still
    // leaves an entire orphan tree in the influence-area graph.
    SupportGeneratorLayersPtr &top_contacts = interface_placer.top_contacts_mutable();
    for (size_t contact_idx = config.raft_layers.size(); contact_idx < top_contacts.size(); ++contact_idx) {
        SupportGeneratorLayer *contact = top_contacts[contact_idx];
        if (contact == nullptr || contact->polygons.empty())
            continue;
        const size_t model_idx = contact_idx + config.z_distance_top_layers + 1 - config.raft_layers.size();
        if (model_idx >= print_object.layer_count())
            continue;
        contact->idx_object_layer_above = model_idx;
        if (area(intersection(contact->polygons, to_polygons(print_object.get_layer(model_idx)->lslices))) <=
            sqr(double(scale_(0.001))))
            contact->polygons.clear();
    }
    for (RichInterfacePlacer::PendingPlacement &placement : placements) {
        rich_interface_placer.publish_tips(std::move(placement));
        throw_on_cancel();
    }

    finalize_raft_contact(print_object, raft_contact_layer_idx, interface_placer.top_contacts_mutable(), move_bounds);
}

static unsigned int move_inside(const Polygons &polygons, Point &from, int distance = 0, int64_t maxDist2 = std::numeric_limits<int64_t>::max())
{
    Point  ret = from;
    double bestDist2 = std::numeric_limits<double>::max();
    auto   bestPoly = static_cast<unsigned int>(-1);
    bool   is_already_on_correct_side_of_boundary = false; // whether [from] is already on the right side of the boundary
    for (unsigned int poly_idx = 0; poly_idx < polygons.size(); ++ poly_idx) {
        const Polygon &poly = polygons[poly_idx];
        if (poly.size() < 2)
            continue;
        Point p0 = poly[poly.size() - 2];
        Point p1 = poly.back();
        // because we compare with vSize2 here (no division by zero), we also need to compare by vSize2 inside the loop
        // to avoid integer rounding edge cases
        bool projected_p_beyond_prev_segment = (p1 - p0).cast<int64_t>().dot((from - p0).cast<int64_t>()) >= (p1 - p0).cast<int64_t>().squaredNorm();
        for (const Point& p2 : poly) {
            // X = A + Normal(B-A) * (((B-A) dot (P-A)) / VSize(B-A));
            //   = A +       (B-A) *  ((B-A) dot (P-A)) / VSize2(B-A);
            // X = P projected on AB
            const Point& a = p1;
            const Point& b = p2;
            const Point& p = from;
            Vec2i64 ab = (b - a).cast<int64_t>();
            Vec2i64 ap = (p - a).cast<int64_t>();
            int64_t ab_length2 = ab.squaredNorm();
            if (ab_length2 <= 0) { //A = B, i.e. the input polygon had two adjacent points on top of each other.
                p1 = p2; //Skip only one of the points.
                continue;
            }
            int64_t dot_prod = ab.dot(ap);
            if (dot_prod <= 0) { // x is projected to before ab
                if (projected_p_beyond_prev_segment) {
                    //  case which looks like:   > .
                    projected_p_beyond_prev_segment = false;
                    Point& x = p1;

                    auto dist2 = (x - p).cast<int64_t>().squaredNorm();
                    if (dist2 < bestDist2) {
                        bestDist2 = dist2;
                        bestPoly = poly_idx;
                        if (distance == 0)
                            ret = x;
                        else {
                            Vec2d  abd   = ab.cast<double>();
                            Vec2d  p1p2  = (p1 - p0).cast<double>();
                            double lab   = abd.norm();
                            double lp1p2 = p1p2.norm();
                            // inward direction irrespective of sign of [distance]
                            Vec2d inward_dir = perp(abd * (scaled<double>(10.0) / lab) + p1p2 * (scaled<double>(10.0) / lp1p2));
                            // MM2INT(10.0) to retain precision for the eventual normalization
                            ret = x + (inward_dir * (distance / inward_dir.norm())).cast<coord_t>();
                            is_already_on_correct_side_of_boundary = inward_dir.dot((p - x).cast<double>()) * distance >= 0;
                        }
                    }
                } else {
                    projected_p_beyond_prev_segment = false;
                    p0 = p1;
                    p1 = p2;
                    continue;
                }
            } else if (dot_prod >= ab_length2) {
                // x is projected to beyond ab
                projected_p_beyond_prev_segment = true;
                p0 = p1;
                p1 = p2;
                continue;
            } else {
                // x is projected to a point properly on the line segment (not onto a vertex). The case which looks like | .
                projected_p_beyond_prev_segment = false;
                Point x = a + (ab.cast<double>() * (double(dot_prod) / double(ab_length2))).cast<coord_t>();
                auto dist2 = (p - x).cast<int64_t>().squaredNorm();
                if (dist2 < bestDist2) {
                    bestDist2 = dist2;
                    bestPoly = poly_idx;
                    if (distance == 0)
                        ret = x;
                    else {
                        Vec2d abd = ab.cast<double>();
                        Vec2d inward_dir = perp(abd * (distance / abd.norm())); // inward or outward depending on the sign of [distance]
                        ret = x + inward_dir.cast<coord_t>();
                        is_already_on_correct_side_of_boundary = inward_dir.dot((p - x).cast<double>()) >= 0;
                    }
                }
            }
            p0 = p1;
            p1 = p2;
        }
    }
    // when the best point is already inside and we're moving inside, or when the best point is already outside and we're moving outside
    if (is_already_on_correct_side_of_boundary) {
        if (bestDist2 < distance * distance)
            from = ret;
        else {
            // from = from; // original point stays unaltered. It is already inside by enough distance
        }
        return bestPoly;
    } else if (bestDist2 < maxDist2) {
        from = ret;
        return bestPoly;
    }
    return -1;
}

static Point move_inside_if_outside(const Polygons &polygons, Point from, int distance = 0, int64_t maxDist2 = std::numeric_limits<int64_t>::max())
{
    if (! contains(polygons, from))
        move_inside(polygons, from);
    return from;
}

/*!
 * \brief Checks if an influence area contains a valid subsection and returns the corresponding metadata and the new Influence area.
 *
 * Calculates an influence areas of the layer below, based on the influence area of one element on the current layer.
 * Increases every influence area by maximum_move_distance_slow. If this is not enough, as in we would change our gracious or to_buildplate status the influence areas are instead increased by maximum_move_distance_slow.
 * Also ensures that increasing the radius of a branch, does not cause it to change its status (like to_buildplate ). If this were the case, the radius is not increased instead.
 *
 * Warning: The used format inside this is different as the SupportElement does not have a valid area member. Instead this area is saved as value of the dictionary. This was done to avoid not needed heap allocations.
 *
 * \param settings[in] Which settings have to be used to check validity.
 * \param layer_idx[in] Number of the current layer.
 * \param parent[in] The metadata of the parents influence area.
 * \param relevant_offset[in] The maximal possible influence area. No guarantee regarding validity with current layer collision required, as it is ensured in-function!
 * \param to_bp_data[out] The part of the Influence area that can reach the buildplate.
 * \param to_model_data[out] The part of the Influence area that do not have to reach the buildplate. This has overlap with new_layer_data.
 * \param increased[out]  Area than can reach all further up support points. No assurance is made that the buildplate or the model can be reached in accordance to the user-supplied settings.
 * \param overspeed[in] How much should the already offset area be offset again. Usually this is 0.
 * \param mergelayer[in] Will the merge method be called on this layer. This information is required as some calculation can be avoided if they are not required for merging.
 * \return A valid support element for the next layer regarding the calculated influence areas. Empty if no influence are can be created using the supplied influence area and settings.
 */
[[nodiscard]] static std::optional<SupportElementState> increase_single_area(
    const TreeModelVolumes      &volumes,
    const TreeSupportSettings   &config,
    const AreaIncreaseSettings  &settings,
    const LayerIndex             layer_idx,
    const SupportElement        &parent,
    const Polygons              &relevant_offset,
    Polygons                    &to_bp_data,
    Polygons                    &to_model_data,
    Polygons                    &increased,
    const coord_t                overspeed,
    const bool                   mergelayer)
{
    SupportElementState current_elem{ SupportElementState::propagate_down(parent.state) };
    Polygons check_layer_data;
    if (settings.increase_radius)
        current_elem.effective_radius_height += 1;
    coord_t radius = support_element_collision_radius(config, current_elem);

    const auto _tiny_area_threshold = tiny_area_threshold();
    if (settings.move) {
        increased = relevant_offset;
        if (overspeed > 0) {
            coord_t safe_movement_distance =
                (current_elem.use_min_xy_dist ? config.xy_min_distance : config.xy_distance) +
                (std::min(config.z_distance_top_layers, config.z_distance_bottom_layers) > 0 ? config.min_feature_size : 0);
            // Inlong:
            // safe_movement_distance is used as the safe_offset_inc() step, so keep it non-zero
            // to preserve branch movement with zero-clearance support settings.
            if (safe_movement_distance == 0)
                safe_movement_distance = scaled<coord_t>(0.1);
            // The difference to ensure that the result not only conforms to wall_restriction, but collision/avoidance is done later.
            // The higher last_safe_step_movement_distance comes exactly from the fact that the collision will be subtracted later.
            increased = safe_offset_inc(increased, overspeed, volumes.getWallRestriction(support_element_collision_radius(config, parent.state), layer_idx, parent.state.use_min_xy_dist),
                safe_movement_distance, safe_movement_distance + radius, 1);
        }
        if (settings.no_error && settings.move)
            // as jtRound has to be used for offsets this simplify is VERY important for performance.
            polygons_simplify(increased, scaled<float>(0.025));
    } else
        // if no movement is done the areas keep parent area as no move == offset(0)
        increased = parent.influence_area;

    if (mergelayer || current_elem.to_buildplate) {
        to_bp_data = safe_union(diff_clipped(increased, volumes.getAvoidance(radius, layer_idx - 1, settings.type, false, settings.use_min_distance)));
        if (! current_elem.to_buildplate && area(to_bp_data) > _tiny_area_threshold) {
            // mostly happening in the tip, but with merges one should check every time, just to be sure.
            current_elem.to_buildplate = true; // sometimes nodes that can reach the buildplate are marked as cant reach, tainting subtrees. This corrects it.
            BOOST_LOG_TRIVIAL(debug) << "Corrected taint leading to a wrong to model value on layer " << layer_idx - 1 << " targeting " <<
                current_elem.target_height << " with radius " << radius;
        }
    }
    if (config.support_rests_on_model) {
        if (mergelayer || current_elem.to_model_gracious)
            to_model_data = safe_union(diff_clipped(increased, volumes.getAvoidance(radius, layer_idx - 1, settings.type, true, settings.use_min_distance)));

        if (!current_elem.to_model_gracious) {
            if (mergelayer && area(to_model_data) >= _tiny_area_threshold) {
                current_elem.to_model_gracious = true;
                BOOST_LOG_TRIVIAL(debug) << "Corrected taint leading to a wrong non gracious value on layer " << layer_idx - 1 << " targeting " <<
                    current_elem.target_height << " with radius " << radius;
            } else
                // Cannot route to gracious areas. Push the tree away from object and route it down anyways.
                to_model_data = safe_union(diff_clipped(increased, volumes.getCollision(radius, layer_idx - 1, settings.use_min_distance)));
        }
    }

    check_layer_data = current_elem.to_buildplate ? to_bp_data : to_model_data;

    if (settings.increase_radius && area(check_layer_data) > _tiny_area_threshold) {
        auto validWithRadius = [&](coord_t next_radius) {
            if (volumes.ceilRadius(next_radius, settings.use_min_distance) <= volumes.ceilRadius(radius, settings.use_min_distance))
                return true;

            Polygons to_bp_data_2;
            if (current_elem.to_buildplate)
                // regular union as output will not be used later => this area should always be a subset of the safe_union one (i think)
                to_bp_data_2 = diff_clipped(increased, volumes.getAvoidance(next_radius, layer_idx - 1, settings.type, false, settings.use_min_distance));
            Polygons to_model_data_2;
            if (config.support_rests_on_model && !current_elem.to_buildplate)
                to_model_data_2 = diff_clipped(increased,
                    current_elem.to_model_gracious ?
                        volumes.getAvoidance(next_radius, layer_idx - 1, settings.type, true, settings.use_min_distance) :
                        volumes.getCollision(next_radius, layer_idx - 1, settings.use_min_distance));
            Polygons check_layer_data_2 = current_elem.to_buildplate ? to_bp_data_2 : to_model_data_2;
            return area(check_layer_data_2) > _tiny_area_threshold;
        };
        coord_t ceil_radius_before = volumes.ceilRadius(radius, settings.use_min_distance);

        if (support_element_collision_radius(config, current_elem) < config.increase_radius_until_radius && support_element_collision_radius(config, current_elem) < support_element_radius(config, current_elem)) {
            coord_t target_radius = std::min(support_element_radius(config, current_elem), config.increase_radius_until_radius);
            coord_t current_ceil_radius = volumes.getRadiusNextCeil(radius, settings.use_min_distance);

            while (current_ceil_radius < target_radius && validWithRadius(volumes.getRadiusNextCeil(current_ceil_radius + 1, settings.use_min_distance)))
                current_ceil_radius = volumes.getRadiusNextCeil(current_ceil_radius + 1, settings.use_min_distance);
            size_t resulting_eff_dtt = current_elem.effective_radius_height;
            while (resulting_eff_dtt + 1 < current_elem.distance_to_top &&
                config.getRadius(resulting_eff_dtt + 1, current_elem.elephant_foot_increases) <= current_ceil_radius &&
                config.getRadius(resulting_eff_dtt + 1, current_elem.elephant_foot_increases) <= support_element_radius(config, current_elem))
                ++ resulting_eff_dtt;
            current_elem.effective_radius_height = resulting_eff_dtt;
        }
        radius = support_element_collision_radius(config, current_elem);

        const coord_t foot_radius_increase = std::max(config.bp_radius_increase_per_layer - config.branch_radius_increase_per_layer, 0.0);
        // Is nearly all of the time 1, but sometimes an increase of 1 could cause the radius to become bigger than recommendedMinRadius,
        // which could cause the radius to become bigger than precalculated.
        double planned_foot_increase = std::min(1.0, double(config.recommendedMinRadius(layer_idx - 1) - support_element_radius(config, current_elem)) / foot_radius_increase);
//FIXME
        bool increase_bp_foot = planned_foot_increase > 0 && current_elem.to_buildplate;
//        bool increase_bp_foot = false;

        if (increase_bp_foot && support_element_radius(config, current_elem) >= config.branch_radius && support_element_radius(config, current_elem) >= config.increase_radius_until_radius)
            if (validWithRadius(config.getRadius(current_elem.effective_radius_height, current_elem.elephant_foot_increases + planned_foot_increase))) {
                current_elem.elephant_foot_increases += planned_foot_increase;
                radius = support_element_collision_radius(config, current_elem);
            }

        if (ceil_radius_before != volumes.ceilRadius(radius, settings.use_min_distance)) {
            if (current_elem.to_buildplate)
                to_bp_data = safe_union(diff_clipped(increased, volumes.getAvoidance(radius, layer_idx - 1, settings.type, false, settings.use_min_distance)));
            if (config.support_rests_on_model && (!current_elem.to_buildplate || mergelayer))
                to_model_data = safe_union(diff_clipped(increased,
                    current_elem.to_model_gracious ?
                        volumes.getAvoidance(radius, layer_idx - 1, settings.type, true, settings.use_min_distance) :
                        volumes.getCollision(radius, layer_idx - 1, settings.use_min_distance)
                ));
            check_layer_data = current_elem.to_buildplate ? to_bp_data : to_model_data;
            if (area(check_layer_data) < _tiny_area_threshold) {
                BOOST_LOG_TRIVIAL(debug) << "Lost area by doing catch up from " << ceil_radius_before << " to radius " <<
                    volumes.ceilRadius(support_element_collision_radius(config, current_elem), settings.use_min_distance);
                tree_supports_show_error("Area lost catching up radius. May not cause visible malformation."sv, true);
            }
        }
    }

    return area(check_layer_data) > _tiny_area_threshold ? std::optional<SupportElementState>(current_elem) : std::optional<SupportElementState>();
}

struct SupportElementInfluenceAreas {
    // All influence areas: both to build plate and model.
    Polygons                        influence_areas;
    // Influence areas just to build plate.
    Polygons                        to_bp_areas;
    // Influence areas just to model.
    Polygons                        to_model_areas;

    void clear() {
        this->influence_areas.clear();
        this->to_bp_areas.clear();
        this->to_model_areas.clear();
    }
};

struct SupportElementMerging {
    SupportElementState                     state;
    void set_state(const SupportElementState &new_state, const TreeSupportSettings &config)
    {
        state = new_state;
        // A layer may compare this element with many unrelated branches. These
        // values depend only on its state and the fixed settings, not the pair.
        radius = support_element_radius(config, state);
        collision_radius = support_element_collision_radius(config, state);
    }
    /*!
     * \brief All elements in the layer above the current one that are supported by this element
     */
    SupportElement::ParentIndices           parents;

    SupportElementInfluenceAreas            areas;
    // Bounding box of all influence areas.
    Eigen::AlignedBox<coord_t, 2>           bbox_data;
    coord_t                                 radius{ 0 };
    coord_t                                 collision_radius{ 0 };

    const Eigen::AlignedBox<coord_t, 2>&    bbox() const { return bbox_data;}
    const Point                             centroid() const { return (bbox_data.min() + bbox_data.max()) / 2; }
    void                                    set_bbox(const BoundingBox& abbox)
        { Point eps { coord_t(SCALED_EPSILON), coord_t(SCALED_EPSILON) }; bbox_data = { abbox.min - eps, abbox.max + eps }; }

    // Called by the AABBTree builder to get an index into the vector of source elements.
    // Not needed, thus zero is returned.
    static size_t                           idx() { return 0; }
};

/*!
 * \brief Increases influence areas as far as required.
 *
 * Calculates influence areas of the layer below, based on the influence areas of the current layer.
 * Increases every influence area by maximum_move_distance_slow. If this is not enough, as in it would change the gracious or to_buildplate status, the influence areas are instead increased by maximum_move_distance.
 * Also ensures that increasing the radius of a branch, does not cause it to change its status (like to_buildplate ). If this were the case, the radius is not increased instead.
 *
 * Warning: The used format inside this is different as the SupportElement does not have a valid area member. Instead this area is saved as value of the dictionary. This was done to avoid not needed heap allocations.
 *
 * \param to_bp_areas[out] Influence areas that can reach the buildplate
 * \param to_model_areas[out] Influence areas that do not have to reach the buildplate. This has overlap with new_layer_data, as areas that can reach the buildplate are also considered valid areas to the model.
 * This redundancy is required if a to_buildplate influence area is allowed to merge with a to model influence area.
 * \param influence_areas[out] Area than can reach all further up support points. No assurance is made that the buildplate or the model can be reached in accordance to the user-supplied settings.
 * \param bypass_merge_areas[out] Influence areas ready to be added to the layer below that do not need merging.
 * \param last_layer[in] Influence areas of the current layer.
 * \param layer_idx[in] Number of the current layer.
 * \param mergelayer[in] Will the merge method be called on this layer. This information is required as some calculation can be avoided if they are not required for merging.
 */
static void increase_areas_one_layer(
    const TreeModelVolumes              &volumes,
    const TreeSupportSettings           &config,
    // New areas at the layer below layer_idx
    std::vector<SupportElementMerging>  &merging_areas,
    // Layer above merging_areas.
    const LayerIndex                     layer_idx,
    // Layer elements above merging_areas.
    SupportElements                     &layer_elements,
    // If false, the merging_areas will not be merged for performance reasons.
    const bool                           mergelayer,
    std::function<void()>                throw_on_cancel)
{
    using AvoidanceType = TreeModelVolumes::AvoidanceType;

    tbb::parallel_for(tbb::blocked_range<size_t>(0, merging_areas.size(), 1),
        [&](const tbb::blocked_range<size_t> &range) {
        for (size_t merging_area_idx = range.begin(); merging_area_idx < range.end(); ++ merging_area_idx) {
            SupportElementMerging   &merging_area   = merging_areas[merging_area_idx];
            assert(merging_area.parents.size() == 1);
            SupportElement          &parent         = layer_elements[merging_area.parents.front()];
            SupportElementState      elem           = SupportElementState::propagate_down(parent.state);
            const Polygons          &wall_restriction =
                // Abstract representation of the model outline. If an influence area would move through it, it could teleport through a wall.
                volumes.getWallRestriction(support_element_collision_radius(config, parent.state), layer_idx, parent.state.use_min_xy_dist);


            Polygons to_bp_data, to_model_data;
            coord_t radius = support_element_collision_radius(config, elem);

            // When the radius increases, the outer "support wall" of the branch will have been moved farther away from the center (as this is the definition of radius).
            // As it is not specified that the support_tree_angle has to be one of the center of the branch, it is here seen as the smaller angle of the outer wall of the branch, to the outer wall of the same branch one layer above.
            // As the branch may have become larger the distance between these 2 walls is smaller than the distance of the center points.
            // These extra distance is added to the movement distance possible for this layer.

            coord_t extra_speed = 5; // The extra speed is added to both movement distances. Also move 5 microns faster than allowed to avoid rounding errors, this may cause issues at VERY VERY small layer heights.
            coord_t extra_slow_speed = 0; // Only added to the slow movement distance.
            const coord_t ceiled_parent_radius = volumes.ceilRadius(support_element_collision_radius(config, parent.state), parent.state.use_min_xy_dist);
            coord_t projected_radius_increased = config.getRadius(parent.state.effective_radius_height + 1, parent.state.elephant_foot_increases);
            coord_t projected_radius_delta = projected_radius_increased - support_element_collision_radius(config, parent.state);

            // When z distance is more than one layer up and down the Collision used to calculate the wall restriction will always include the wall (and not just the xy_min_distance) of the layer above and below like this (d = blocked area because of z distance):
            /*
             *  layer z+1:dddddiiiiiioooo
             *  layer z+0:xxxxxdddddddddd
             *  layer z-1:dddddxxxxxxxxxx
             *  For more detailed visualisation see calculateWallRestrictions
             */
            coord_t safe_movement_distance =
                (elem.use_min_xy_dist ? config.xy_min_distance : config.xy_distance) +
                (std::min(config.z_distance_top_layers, config.z_distance_bottom_layers) > 0 ? config.min_feature_size : 0);

            // safe_movement_distance is used as a divisor and as the safe_offset_inc() step,
            // so keep it non-zero to avoid division by zero and preserve branch movement.
            if (safe_movement_distance == 0)
                safe_movement_distance = scaled<coord_t>(0.1);

            if (ceiled_parent_radius == volumes.ceilRadius(projected_radius_increased, parent.state.use_min_xy_dist) ||
                projected_radius_increased < config.increase_radius_until_radius)
                // If it is guaranteed possible to increase the radius, the maximum movement speed can be increased, as it is assumed that the maximum movement speed is the one of the slower moving wall
                extra_speed += projected_radius_delta;
            else
                // if a guaranteed radius increase is not possible, only increase the slow speed
                // Ensure that the slow movement distance can not become larger than the fast one.
                extra_slow_speed += std::min(projected_radius_delta, (config.maximum_move_distance + extra_speed) - (config.maximum_move_distance_slow + extra_slow_speed));

            if (config.layer_start_bp_radius > layer_idx &&
                config.recommendedMinRadius(layer_idx - 1) < config.getRadius(elem.effective_radius_height + 1, elem.elephant_foot_increases)) {
                // can guarantee elephant foot radius increase
                if (ceiled_parent_radius == volumes.ceilRadius(config.getRadius(parent.state.effective_radius_height + 1, parent.state.elephant_foot_increases + 1), parent.state.use_min_xy_dist))
                    extra_speed += config.bp_radius_increase_per_layer;
                else
                    extra_slow_speed += std::min(coord_t(config.bp_radius_increase_per_layer),
                                                 config.maximum_move_distance - (config.maximum_move_distance_slow + extra_slow_speed));
            }

            const coord_t fast_speed = config.maximum_move_distance + extra_speed;
            const coord_t slow_speed = config.maximum_move_distance_slow + extra_speed + extra_slow_speed;

            Polygons offset_slow, offset_fast;

            bool add = false;
            bool bypass_merge = false;
            constexpr bool increase_radius = true, no_error = true, use_min_radius = true, move = true; // aliases for better readability

            // Determine in which order configurations are checked if they result in a valid influence area. Check will stop if a valid area is found
            std::vector<AreaIncreaseSettings> order;
            auto insertSetting = [&](AreaIncreaseSettings settings, bool back) {
                if (std::find(order.begin(), order.end(), settings) == order.end()) {
                    if (back)
                        order.emplace_back(settings);
                    else
                        order.insert(order.begin(), settings);
                }
            };

            const bool parent_moved_slow = elem.last_area_increase.increase_speed < config.maximum_move_distance;
            const bool avoidance_speed_mismatch = parent_moved_slow && elem.last_area_increase.type != AvoidanceType::Slow;
            if (elem.last_area_increase.move && elem.last_area_increase.no_error && elem.can_use_safe_radius && !mergelayer &&
                !avoidance_speed_mismatch && (elem.distance_to_top >= config.tip_layers || parent_moved_slow)) {
                // assume that the avoidance type that was best for the parent is best for me. Makes this function about 7% faster.
                insertSetting({ elem.last_area_increase.type, elem.last_area_increase.increase_speed < config.maximum_move_distance ? slow_speed : fast_speed,
                    increase_radius, elem.last_area_increase.no_error, !use_min_radius, elem.last_area_increase.move }, true);
                insertSetting({ elem.last_area_increase.type, elem.last_area_increase.increase_speed < config.maximum_move_distance ? slow_speed : fast_speed,
                    !increase_radius, elem.last_area_increase.no_error, !use_min_radius, elem.last_area_increase.move }, true);
            }
            // branch may still go though a hole, so a check has to be done whether the hole was already passed, and the regular avoidance can be used.
            if (!elem.can_use_safe_radius) {
                // if the radius until which it is always increased can not be guaranteed, move fast. This is to avoid holes smaller than the real branch radius.
                // This does not guarantee the avoidance of such holes, but ensures they are avoided if possible.
                // order.emplace_back(AvoidanceType::Slow,!increase_radius,no_error,!use_min_radius,move);
                insertSetting({ AvoidanceType::Slow, slow_speed, increase_radius, no_error, !use_min_radius, !move }, true); // did we go through the hole
                // in many cases the definition of hole is overly restrictive, so to avoid unnecessary fast movement in the tip, it is ignored there for a bit.
                // This CAN cause a branch to go though a hole it otherwise may have avoided.
                if (elem.distance_to_top < round_up_divide(config.tip_layers, size_t(2)))
                    insertSetting({ AvoidanceType::Fast, slow_speed, increase_radius, no_error, !use_min_radius, !move }, true);
                insertSetting({ AvoidanceType::FastSafe, fast_speed, increase_radius, no_error, !use_min_radius, !move }, true); // did we manage to avoid the hole
                insertSetting({ AvoidanceType::FastSafe, fast_speed, !increase_radius, no_error, !use_min_radius, move }, true);
                insertSetting({ AvoidanceType::Fast, fast_speed, !increase_radius, no_error, !use_min_radius, move }, true);
            } else {
                insertSetting({ AvoidanceType::Slow, slow_speed, increase_radius, no_error, !use_min_radius, move }, true);
                // while moving fast to be able to increase the radius (b) may seems preferable (over a) this can cause the a sudden skip in movement,
                // which looks similar to a layer shift and can reduce stability.
                // as such idx have chosen to only use the user setting for radius increases as a friendly recommendation.
                insertSetting({ AvoidanceType::Slow, slow_speed, !increase_radius, no_error, !use_min_radius, move }, true); // a
                if (elem.distance_to_top < config.tip_layers)
                    insertSetting({ AvoidanceType::FastSafe, slow_speed, increase_radius, no_error, !use_min_radius, move }, true);
                insertSetting({ AvoidanceType::FastSafe, fast_speed, increase_radius, no_error, !use_min_radius, move }, true); // b
                insertSetting({ AvoidanceType::FastSafe, fast_speed, !increase_radius, no_error, !use_min_radius, move }, true);
            }

            if (elem.use_min_xy_dist) {
                std::vector<AreaIncreaseSettings> new_order;
                // if the branch currently has to use min_xy_dist check if the configuration would also be valid
                // with the regular xy_distance before checking with use_min_radius (Only happens when Support Distance priority is z overrides xy )
                for (AreaIncreaseSettings settings : order) {
                    new_order.emplace_back(settings);
                    new_order.push_back({ settings.type, settings.increase_speed, settings.increase_radius, settings.no_error, use_min_radius, settings.move });
                }
                order = new_order;
            }
            if (elem.to_buildplate || (elem.to_model_gracious && intersection(parent.influence_area, volumes.getPlaceableAreas(radius, layer_idx, throw_on_cancel)).empty())) {
                // error case
                // it is normal that we wont be able to find a new area at some point in time if we wont be able to reach layer 0 aka have to connect with the model
                insertSetting({ AvoidanceType::Fast, fast_speed, !increase_radius, !no_error, elem.use_min_xy_dist, move }, true);
            }
            if (elem.distance_to_top < elem.dont_move_until && elem.can_use_safe_radius) // only do not move when holes would be avoided in every case.
                // Only do not move when already in a no hole avoidance with the regular xy distance.
                insertSetting({ AvoidanceType::Slow, 0, increase_radius, no_error, !use_min_radius, !move }, false);

            Polygons inc_wo_collision;
            // Check whether it is faster to calculate the area increased with the fast speed independently from the slow area, or time could be saved by reusing the slow area to calculate the fast one.
            // Calculated by comparing the steps saved when calcualting idependently with the saved steps when not.
            bool offset_independant_faster = radius / safe_movement_distance - int(config.maximum_move_distance + extra_speed < radius + safe_movement_distance) >
                                             round_up_divide((extra_speed + extra_slow_speed + config.maximum_move_distance_slow), safe_movement_distance);
            for (const AreaIncreaseSettings &settings : order) {
                if (settings.move) {
                    if (offset_slow.empty() && (settings.increase_speed == slow_speed || ! offset_independant_faster)) {
                        // offsetting in 2 steps makes our offsetted area rounder preventing (rounding) errors created by to pointy areas. At this point one can see that the Polygons class
                        // was never made for precision in the single digit micron range.
                        offset_slow = safe_offset_inc(parent.influence_area, extra_speed + extra_slow_speed + config.maximum_move_distance_slow,
                            wall_restriction, safe_movement_distance, offset_independant_faster ? safe_movement_distance + radius : 0, 2);
                    }
                    if (offset_fast.empty() && settings.increase_speed != slow_speed) {
                        if (offset_independant_faster)
                            offset_fast = safe_offset_inc(parent.influence_area, extra_speed + config.maximum_move_distance,
                                wall_restriction, safe_movement_distance, offset_independant_faster ? safe_movement_distance + radius : 0, 1);
                        else {
                            const coord_t delta_slow_fast = config.maximum_move_distance - (config.maximum_move_distance_slow + extra_slow_speed);
                            offset_fast = safe_offset_inc(offset_slow, delta_slow_fast, wall_restriction, safe_movement_distance, safe_movement_distance + radius, offset_independant_faster ? 2 : 1);
                        }
                    }
                }
                std::optional<SupportElementState> result;
                inc_wo_collision.clear();
                if (!settings.no_error) {
                    // ERROR CASE
                    // if the area becomes for whatever reason something that clipper sees as a line, offset would stop working, so ensure that even if it would be a line wrongly, it still actually has an area that can be increased
                    Polygons lines_offset = offset(to_polylines(parent.influence_area), scaled<float>(0.005), jtMiter, 1.2);
                    Polygons base_error_area = union_(parent.influence_area, lines_offset);
                    result = increase_single_area(volumes, config, settings, layer_idx, parent,
                        base_error_area, to_bp_data, to_model_data, inc_wo_collision, (config.maximum_move_distance + extra_speed) * 1.5, mergelayer);
                    if (!result) {
#ifdef TREE_SUPPORT_SHOW_ERRORS
                        BOOST_LOG_TRIVIAL(error)
#else // TREE_SUPPORT_SHOW_ERRORS
                        BOOST_LOG_TRIVIAL(warning)
#endif // TREE_SUPPORT_SHOW_ERRORS
                            << "Influence area could not be increased! Data about the Influence area: "
                               "Radius: " << radius << " at layer: " << layer_idx - 1 << " NextTarget: " << elem.layer_idx << " Distance to top: " << elem.distance_to_top <<
                               " Elephant foot increases " << elem.elephant_foot_increases << " use_min_xy_dist " << elem.use_min_xy_dist << " to buildplate " << elem.to_buildplate <<
                               " gracious " << elem.to_model_gracious << " safe " << elem.can_use_safe_radius << " until move " << elem.dont_move_until << " \n "
                               "Parent " << &parent << ": Radius: " << support_element_collision_radius(config, parent.state) << " at layer: " << layer_idx << " NextTarget: " << parent.state.layer_idx <<
                               " Distance to top: " << parent.state.distance_to_top << " Elephant foot increases " << parent.state.elephant_foot_increases << "  use_min_xy_dist " << parent.state.use_min_xy_dist <<
                               " to buildplate " << parent.state.to_buildplate << " gracious " << parent.state.to_model_gracious << " safe " << parent.state.can_use_safe_radius << " until move " << parent.state.dont_move_until;
                        tree_supports_show_error("Potentially lost branch!"sv, true);
                    }
#ifdef TREE_SUPPORTS_TRACK_LOST
                    if (result)
                        result->lost = true;
#endif // TREE_SUPPORTS_TRACK_LOST
                } else
                    result = increase_single_area(volumes, config, settings, layer_idx, parent,
                        settings.increase_speed == slow_speed ? offset_slow : offset_fast, to_bp_data, to_model_data, inc_wo_collision, 0, mergelayer);

                if (result) {
                    elem = *result;
                    radius = support_element_collision_radius(config, elem);
                    elem.last_area_increase = settings;
                    add = true;
                    // do not merge if the branch should not move or the priority has to be to get farther away from the model.
                    bypass_merge = !settings.move || (settings.use_min_distance && elem.distance_to_top < config.tip_layers);
                    if (settings.move)
                        elem.dont_move_until = 0;
                    else
                        elem.result_on_layer = parent.state.result_on_layer;

                    elem.can_use_safe_radius = settings.type != AvoidanceType::Fast;

                    if (!settings.use_min_distance)
                        elem.use_min_xy_dist = false;
                    if (!settings.no_error)
#ifdef TREE_SUPPORT_SHOW_ERRORS
                        BOOST_LOG_TRIVIAL(error)
#else // TREE_SUPPORT_SHOW_ERRORS
                        BOOST_LOG_TRIVIAL(info)
#endif // TREE_SUPPORT_SHOW_ERRORS
                            << "Trying to keep area by moving faster than intended: Success";
                    break;
                } else if (!settings.no_error)
                    BOOST_LOG_TRIVIAL(warning) << "Trying to keep area by moving faster than intended: FAILURE! WRONG BRANCHES LIKLY!";
            }

            if (add) {
                // Union seems useless, but some rounding errors somewhere can cause to_bp_data to be slightly bigger than it should be.
                assert(! inc_wo_collision.empty() || ! to_bp_data.empty() || ! to_model_data.empty());
                Polygons max_influence_area = safe_union(
                    diff_clipped(inc_wo_collision, volumes.getCollision(radius, layer_idx - 1, elem.use_min_xy_dist)),
                    safe_union(to_bp_data, to_model_data));
                merging_area.set_state(elem, config);
                assert(!max_influence_area.empty());
                merging_area.set_bbox(get_extents(max_influence_area));
                merging_area.areas.influence_areas = std::move(max_influence_area);
                if (! bypass_merge) {
                    if (elem.to_buildplate)
                        merging_area.areas.to_bp_areas = std::move(to_bp_data);
                    if (config.support_rests_on_model)
                        merging_area.areas.to_model_areas = std::move(to_model_data);
                }
            } else {
                // If the bottom most point of a branch is set, later functions will assume that the position is valid, and ignore it.
                // But as branches connecting with the model that are to small have to be culled, the bottom most point has to be not set.
                // A point can be set on the top most tip layer (maybe more if it should not move for a few layers).
                parent.state.result_on_layer_reset();
                parent.state.to_model_gracious = false;
#ifdef TREE_SUPPORTS_TRACK_LOST
                parent.state.verylost = true;
#endif // TREE_SUPPORTS_TRACK_LOST
            }

            throw_on_cancel();
        }
    }, tbb::simple_partitioner());
}

[[nodiscard]] static SupportElementState merge_support_element_states(
    const SupportElementState &first, const SupportElementState &second, const Point &next_position, const coord_t layer_idx,
    const TreeSupportSettings &config)
{
    SupportElementState out;
    out.next_position   = next_position;
    out.layer_idx       = layer_idx;
    out.use_min_xy_dist = first.use_min_xy_dist || second.use_min_xy_dist;
    out.supports_roof   = first.supports_roof || second.supports_roof;
    out.dont_move_until = std::max(first.dont_move_until, second.dont_move_until);
    out.can_use_safe_radius = first.can_use_safe_radius || second.can_use_safe_radius;
    // Preserve the deepest outstanding roof recovery request across merged sub-branches.
    out.set_pending_roof_recovery(
        std::max(first.missing_roof_layers, second.missing_roof_layers),
        std::max(first.roof_recovery_dtt, second.roof_recovery_dtt));
    out.skip_ovalisation = false;
    if (first.target_height > second.target_height) {
        out.target_height   = first.target_height;
        out.target_position = first.target_position;
    } else {
        out.target_height   = second.target_height;
        out.target_position = second.target_position;
    }
    out.effective_radius_height = std::max(first.effective_radius_height, second.effective_radius_height);
    out.distance_to_top = std::max(first.distance_to_top, second.distance_to_top);

    out.to_buildplate = first.to_buildplate && second.to_buildplate;
    out.to_model_gracious = first.to_model_gracious && second.to_model_gracious; // valid as we do not merge non-gracious with gracious

    out.elephant_foot_increases = 0;
    if (config.bp_radius_increase_per_layer > 0) {
        coord_t foot_increase_radius = std::abs(std::max(support_element_collision_radius(config, second), support_element_collision_radius(config, first)) - support_element_collision_radius(config, out));
        // elephant_foot_increases has to be recalculated, as when a smaller tree with a larger elephant_foot_increases merge with a larger branch
        // the elephant_foot_increases may have to be lower as otherwise the radius suddenly increases. This results often in a non integer value.
        out.elephant_foot_increases = foot_increase_radius / (config.bp_radius_increase_per_layer - config.branch_radius_increase_per_layer);
    }

    // set last settings to the best out of both parents. If this is wrong, it will only cause a small performance penalty instead of weird behavior.
    out.last_area_increase = {
        std::min(first.last_area_increase.type, second.last_area_increase.type),
        std::min(first.last_area_increase.increase_speed, second.last_area_increase.increase_speed),
        first.last_area_increase.increase_radius || second.last_area_increase.increase_radius,
        first.last_area_increase.no_error || second.last_area_increase.no_error,
        first.last_area_increase.use_min_distance && second.last_area_increase.use_min_distance,
        first.last_area_increase.move || second.last_area_increase.move };

    return out;
}

static bool merge_influence_areas_two_elements(
    const TreeModelVolumes &volumes, const TreeSupportSettings &config, const LayerIndex layer_idx,
    SupportElementMerging &dst, SupportElementMerging &src)
{
    // Don't merge gracious with a non gracious area as bad placement could negatively impact reliability of the whole subtree.
    const bool merging_gracious_and_non_gracious = dst.state.to_model_gracious != src.state.to_model_gracious;
    // Could cause some issues with the increase of one area, as it is assumed that if the smaller is increased
    // by the delta to the larger it is engulfed by it already. But because a different collision
    // may be removed from the in draw_area() generated circles, this assumption could be wrong.
    const bool merging_min_and_regular_xy        = dst.state.use_min_xy_dist != src.state.use_min_xy_dist;

    if (merging_gracious_and_non_gracious || merging_min_and_regular_xy)
        return false;

    const bool dst_radius_bigger = dst.collision_radius > src.collision_radius;
    const SupportElementMerging &smaller_rad = dst_radius_bigger ? src : dst;
    const SupportElementMerging &bigger_rad  = dst_radius_bigger ? dst : src;
    const coord_t real_radius_delta = std::abs(bigger_rad.radius - smaller_rad.radius);
    {
        // Testing intersection of bounding boxes.
        // Expand the smaller radius branch bounding box to match the lambda intersect_small_with_bigger() below.
        // Because the lambda intersect_small_with_bigger() applies a rounded offset, a snug offset of the bounding box
        // is sufficient. On the other side, if a mitered offset was used by the lambda,
        // the bounding box expansion would have to account for the mitered extension of the sharp corners.
        Eigen::AlignedBox<coord_t, 2> smaller_bbox = smaller_rad.bbox();
        smaller_bbox.min() -= Point{ real_radius_delta, real_radius_delta };
        smaller_bbox.max() += Point{ real_radius_delta, real_radius_delta };
        if (! smaller_bbox.intersects(bigger_rad.bbox()))
            return false;
    }

    // Accumulator of a radius increase of a "to model" branch by merging in a "to build plate" branch.
    coord_t increased_to_model_radius = 0;
    const bool merging_to_bp                     = dst.state.to_buildplate && src.state.to_buildplate;
    if (! merging_to_bp) {
        // Get the real radius increase as the user does not care for the collision model.
        if (dst.state.to_buildplate != src.state.to_buildplate) {
            // Merging a "to build plate" branch with a "to model" branch.
            // Don't allow merging a thick "to build plate" branch into a thinner "to model" branch.
            const coord_t rdst = dst.radius;
            const coord_t rsrc = src.radius;
            if (dst.state.to_buildplate) {
                if (rsrc < rdst)
                    increased_to_model_radius = src.state.increased_to_model_radius + rdst - rsrc;
            } else {
                if (rsrc > rdst)
                    increased_to_model_radius = dst.state.increased_to_model_radius + rsrc - rdst;
            }
            if (increased_to_model_radius > config.max_to_model_radius_increase)
                return false;
        }
        // if a merge could place a stable branch on unstable ground, would be increasing the radius further
        // than allowed to when merging to model and to_bp trees or would merge to model before it is known
        // they will even been drawn the merge is skipped
        if (! dst.state.supports_roof && ! src.state.supports_roof &&
            std::max(src.state.distance_to_top, dst.state.distance_to_top) < config.min_dtt_to_model)
            return false;
    }

    // Area of the bigger radius is used to ensure correct placement regarding the relevant avoidance,
    // so if that would change an invalid area may be created.
    if (! bigger_rad.state.can_use_safe_radius && smaller_rad.state.can_use_safe_radius)
        return false;

    // the bigger radius is used to verify that the area is still valid after the increase with the delta.
    // If there were a point where the big influence area could be valid with can_use_safe_radius
    // the element would already be can_use_safe_radius.
    // the smaller radius, which gets increased by delta may reach into the area where use_min_xy_dist is no longer required.
    const bool use_min_radius = bigger_rad.state.use_min_xy_dist && smaller_rad.state.use_min_xy_dist;

    // The idea is that the influence area with the smaller collision radius is increased by the radius difference.
    // If this area has any intersections with the influence area of the larger collision radius, a branch (of the larger collision radius) placed in this intersection, has already engulfed the branch of the smaller collision radius.
    // Because of this a merge may happen even if the influence areas (that represent possible center points of branches) do not intersect yet.
    // Remember that collision radius <= real radius as otherwise this assumption would be false.
    const coord_t   smaller_collision_radius    = smaller_rad.collision_radius;
    const Polygons &collision                   = volumes.getCollision(smaller_collision_radius, layer_idx - 1, use_min_radius);
    auto            intersect_small_with_bigger = [real_radius_delta, smaller_collision_radius, &collision, &config](const Polygons &small, const Polygons &bigger) {
        return intersection(
            safe_offset_inc(
                small, real_radius_delta, collision,
                // -3 avoids possible rounding errors
                2 * (config.xy_distance + smaller_collision_radius - 3), 0, 0),
            bigger);
    };
//#define TREES_MERGE_RATHER_LATER
    Polygons intersect = 
#ifdef TREES_MERGE_RATHER_LATER
        intersection(
#else
        intersect_small_with_bigger(            
#endif
        merging_to_bp ? smaller_rad.areas.to_bp_areas : smaller_rad.areas.to_model_areas,
        merging_to_bp ? bigger_rad.areas.to_bp_areas : bigger_rad.areas.to_model_areas);

    const auto _tiny_area_threshold = tiny_area_threshold();
    // dont use empty as a line is not empty, but for this use-case it very well may be (and would be one layer down as union does not keep lines)
    // check if the overlap is large enough (Small ares tend to attract rounding errors in clipper).
    if (area(intersect) <= _tiny_area_threshold)
        return false;

    // While 0.025 was guessed as enough, i did not have reason to change it.
    if (area(offset(intersect, scaled<float>(-0.025), jtMiter, 1.2)) <= _tiny_area_threshold)
        return false;

#ifdef TREES_MERGE_RATHER_LATER
    intersect = 
        intersect_small_with_bigger(
            merging_to_bp ? smaller_rad.areas.to_bp_areas : smaller_rad.areas.to_model_areas,
            merging_to_bp ? bigger_rad.areas.to_bp_areas : bigger_rad.areas.to_model_areas);
#endif

    // Do the actual merge now that the branches are confirmed to be able to intersect.
    // calculate which point is closest to the point of the last merge (or tip center if no merge above it has happened)
    // used at the end to estimate where to best place the branch on the bottom most layer
    // could be replaced with a random point inside the new area
    Point new_pos = move_inside_if_outside(intersect, dst.state.next_position);

    SupportElementState new_state = merge_support_element_states(dst.state, src.state, new_pos, layer_idx - 1, config);
    new_state.increased_to_model_radius = increased_to_model_radius == 0 ?
        // increased_to_model_radius was not set yet. Propagate maximum.
        std::max(dst.state.increased_to_model_radius, src.state.increased_to_model_radius) :
        increased_to_model_radius;

    // Rather unioning with "intersect" due to some rounding errors.
    Polygons influence_areas = safe_union(
        intersect_small_with_bigger(smaller_rad.areas.influence_areas, bigger_rad.areas.influence_areas),
        intersect);

    Polygons to_model_areas;
    if (merging_to_bp && config.support_rests_on_model)
        to_model_areas = new_state.to_model_gracious ?
            // Rather unioning with "intersect" due to some rounding errors.
            safe_union(
                intersect_small_with_bigger(smaller_rad.areas.to_model_areas, bigger_rad.areas.to_model_areas),
                intersect) :
            influence_areas;

    dst.parents.insert(dst.parents.end(), src.parents.begin(), src.parents.end());
    dst.set_state(new_state, config);
    dst.areas.influence_areas = std::move(influence_areas);
    dst.areas.to_bp_areas.clear();
    dst.areas.to_model_areas.clear();
    if (merging_to_bp) {
        dst.areas.to_bp_areas = std::move(intersect);
        if (config.support_rests_on_model)
            dst.areas.to_model_areas = std::move(to_model_areas);
    } else
        dst.areas.to_model_areas = std::move(intersect);
    // Update the bounding box.
    BoundingBox bbox(get_extents(dst.areas.influence_areas));
    bbox.merge(get_extents(dst.areas.to_bp_areas));
    bbox.merge(get_extents(dst.areas.to_model_areas));
    dst.set_bbox(bbox);
    // Clear the source data.
    src.areas.clear();
    src.parents.clear();
    return true;
}

/*!
 * \brief Merges Influence Areas if possible.
 *
 * Branches which do overlap have to be merged. This helper merges all elements in input with the elements into reduced_new_layer.
 * Elements in input_aabb are merged together if possible, while elements reduced_new_layer_aabb are not checked against each other.
 *
 * \param reduced_aabb[in,out] The already processed elements.
 * \param input_aabb[in] Not yet processed elements
 * \param to_bp_areas[in] The Elements of the current Layer that will reach the buildplate. Value is the influence area where the center of a circle of support may be placed.
 * \param to_model_areas[in] The Elements of the current Layer that do not have to reach the buildplate. Also contains main as every element that can reach the buildplate is not forced to.
 * Value is the influence area where the center of a circle of support may be placed.
 * \param influence_areas[in] The influence areas without avoidance removed.
 * \param insert_bp_areas[out] Elements to be inserted into the main dictionary after the Helper terminates.
 * \param insert_model_areas[out] Elements to be inserted into the secondary dictionary after the Helper terminates.
 * \param insert_influence[out] Elements to be inserted into the dictionary containing the largest possibly valid influence area (ignoring if the area may not be there because of avoidance)
 * \param erase[out] Elements that should be deleted from the above dictionaries.
 * \param layer_idx[in] The Index of the current Layer.
 */

static SupportElementMerging* merge_influence_areas_leaves(
    const TreeModelVolumes &volumes, const TreeSupportSettings &config, const LayerIndex layer_idx,
    SupportElementMerging * const dst_begin, SupportElementMerging *dst_end)
{
    // Merging at the lowest level of the AABB tree. Checking one against each other, O(n^2).
    assert(dst_begin < dst_end);
    for (SupportElementMerging *i = dst_begin; i + 1 < dst_end;) {
        for (SupportElementMerging *j = i + 1; j != dst_end;)
            if (merge_influence_areas_two_elements(volumes, config, layer_idx, *i, *j)) {
                // i was merged with j, j is empty.
                if (j != -- dst_end)
                    *j = std::move(*dst_end);
                goto merged;
            } else
                ++ j;
        // not merged
        ++ i;
    merged:
        ;
    }
    return dst_end;
}

static SupportElementMerging* merge_influence_areas_two_sets(
    const TreeModelVolumes &volumes, const TreeSupportSettings &config, const LayerIndex layer_idx,
    SupportElementMerging * const dst_begin, SupportElementMerging *       dst_end,
    SupportElementMerging *       src_begin, SupportElementMerging * const src_end)
{
    // Merging src into dst.
    // Areas of src should not overlap with areas of another elements of src.
    // Areas of dst should not overlap with areas of another elements of dst.
    // The memory from dst_begin to src_end is reserved for the merging operation,
    // src follows dst.
    assert(src_begin < src_end);
    assert(dst_begin < dst_end);
    assert(dst_end <= src_begin);
    for (SupportElementMerging *src = src_begin; src != src_end; ++ src) {
        SupportElementMerging         *dst      = dst_begin;
        SupportElementMerging         *merged   = nullptr;
        for (; dst != dst_end; ++ dst)
            if (merge_influence_areas_two_elements(volumes, config, layer_idx, *dst, *src)) {
                merged = dst ++;
                if (src != src_begin)
                    // Compactify src.
                    *src = std::move(*src_begin);
                ++ src_begin;
                break;
            }
        for (; dst != dst_end;)
            if (merge_influence_areas_two_elements(volumes, config, layer_idx, *merged, *dst)) {
                // Compactify dst.
                if (dst != -- dst_end)
                    *dst = std::move(*dst_end);
            } else
                ++ dst;
    }
    // Compactify src elements that were not merged with dst to the end of dst.
    assert(dst_end <= src_begin);
    if (dst_end == src_begin)
        dst_end = src_end;
    else
        while (src_begin != src_end)
            *dst_end ++ = std::move(*src_begin ++);

    return dst_end;
}

/*!
 * \brief Merges Influence Areas at one layer if possible.
 *
 * Branches which do overlap have to be merged. This manages the helper and uses a divide and conquer approach to parallelize this problem. This parallelization can at most accelerate the merging by a factor of 2.
 *
 * \param to_bp_areas[in] The Elements of the current Layer that will reach the buildplate.
 *  Value is the influence area where the center of a circle of support may be placed.
 * \param to_model_areas[in] The Elements of the current Layer that do not have to reach the buildplate. Also contains main as every element that can reach the buildplate is not forced to.
 *  Value is the influence area where the center of a circle of support may be placed.
 * \param influence_areas[in] The Elements of the current Layer without avoidances removed. This is the largest possible influence area for this layer.
 *  Value is the influence area where the center of a circle of support may be placed.
 * \param layer_idx[in] The current layer.
 */
static void merge_influence_areas(
    const TreeModelVolumes             &volumes,
    const TreeSupportSettings          &config,
    const LayerIndex                    layer_idx,
    std::vector<SupportElementMerging> &influence_areas,
    std::function<void()>               throw_on_cancel)
{
    const size_t input_size = influence_areas.size();
    if (input_size == 0)
        return;

    // Merging by divide & conquer.
    // The majority of time is consumed by Clipper polygon operations, intersection is accelerated by bounding boxes.
    // Sorting input into an AABB tree helps to perform most of the intersections at first iterations,
    // thus reducing computation when merging larger subtrees.
    // The actual merge logic is found in merge_influence_areas_two_sets.

    // Build an AABB tree over the influence areas.
    //FIXME A full tree does not need to be built, the lowest level branches will be always bucketed.
    // However the additional time consumed is negligible.
    AABBTreeIndirect::Tree<2, coord_t> tree;
    // Sort influence_areas in place.
    tree.build_modify_input(influence_areas);

    throw_on_cancel();

    // Prepare the initial buckets as ranges of influence areas. The initial buckets contain power of 2 influence areas to follow
    // the branching of the AABB tree.
    // Vectors of ranges of influence areas, following the branching of the AABB tree:
    std::vector<std::pair<SupportElementMerging*, SupportElementMerging*>> buckets;
    // Initial number of buckets for 1st round of merging.
    size_t num_buckets_initial;
    {
        // How many buckets per first merge iteration?
        // Fixed at 4: merging is not associative, so sizing buckets off max_concurrency() made
        // results depend on the core count of the slicing machine.
        const size_t bucket_size     = 4;
        num_buckets_initial          = (input_size + 2) / 4;
        // Fill in the buckets.
        SupportElementMerging *it = influence_areas.data();
        // Reserve one more bucket to keep a single influence area which will not be merged in the first iteration.
        buckets.reserve(num_buckets_initial + 1);
        for (size_t i = 0; i < num_buckets_initial; ++ i, it += bucket_size)
            buckets.emplace_back(std::make_pair(it, it + bucket_size));
        SupportElementMerging *it_end = influence_areas.data() + influence_areas.size();
        if (buckets.back().second >= it_end) {
            // Last bucket is less than size 4, but bigger than size 1.
            buckets.back().second = std::min(buckets.back().second, it_end);
        } else {
            // Last bucket is size 1, it will not be merged in the first iteration.
            assert(it + 1 == it_end);
            buckets.emplace_back(std::make_pair(it, it_end));
        }
    }

    // 1st merge iteration, merge one with each other.
    tbb::parallel_for(tbb::blocked_range<size_t>(0, num_buckets_initial),
        [&](const tbb::blocked_range<size_t> &range) {
        for (size_t idx = range.begin(); idx < range.end(); ++ idx) {
            // Merge bucket_count adjacent to each other, merging uneven bucket numbers into even buckets
            buckets[idx].second = merge_influence_areas_leaves(volumes, config, layer_idx, buckets[idx].first, buckets[idx].second);
            throw_on_cancel();
        }
    });

    // Further merge iterations, merging one AABB subtree with another one, hopefully minimizing intersections between the elements
    // of each of the subtree.
    while (buckets.size() > 1) {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, buckets.size() / 2),
            [&](const tbb::blocked_range<size_t> &range) {
            for (size_t idx = range.begin(); idx < range.end(); ++ idx) {
                const size_t bucket_pair_idx = idx * 2;
                // Merge bucket_count adjacent to each other, merging uneven bucket numbers into even buckets
                buckets[bucket_pair_idx].second = merge_influence_areas_two_sets(volumes, config, layer_idx,
                    buckets[bucket_pair_idx].first, buckets[bucket_pair_idx].second,
                    buckets[bucket_pair_idx + 1].first, buckets[bucket_pair_idx + 1].second);
                throw_on_cancel();
            }
        });
        // Remove odd buckets, which were merged into even buckets.
        size_t new_size = (buckets.size() + 1) / 2;
        for (size_t i = 1; i < new_size; ++ i)
            buckets[i] = std::move(buckets[i * 2]);
        buckets.erase(buckets.begin() + new_size, buckets.end());
    }
}

/*!
 * \brief Propagates influence downwards, and merges overlapping ones.
 *
 * \param move_bounds[in,out] All currently existing influence areas
 */
static void create_layer_pathing(const TreeModelVolumes &volumes, const TreeSupportSettings &config, std::vector<SupportElements> &move_bounds, std::function<void()> throw_on_cancel)
{
#ifdef SLIC3R_TREESUPPORTS_PROGRESS
    const double data_size_inverse = 1 / double(move_bounds.size());
    double progress_total = TREE_PROGRESS_PRECALC_AVO + TREE_PROGRESS_PRECALC_COLL + TREE_PROGRESS_GENERATE_NODES;
#endif // SLIC3R_TREESUPPORTS_PROGRESS

    auto dur_inc   = std::chrono::duration_values<std::chrono::nanoseconds>::zero();
    auto dur_total = std::chrono::duration_values<std::chrono::nanoseconds>::zero();

    LayerIndex last_merge_layer_idx = move_bounds.size();
    bool new_element = false;
    const auto _tiny_area_threshold = tiny_area_threshold();

    // Ensures at least one merge operation per 3mm height, 50 layers, 1 mm movement of slow speed or 5mm movement of fast speed (whatever is lowest). Values were guessed.
    size_t max_merge_every_x_layers = std::min(std::min(5000 / (std::max(config.maximum_move_distance, coord_t(100))), 1000 / std::max(config.maximum_move_distance_slow, coord_t(20))), 3000 / config.layer_height);
    size_t merge_every_x_layers = 1;
    // Calculate the influence areas for each layer below (Top down)
    // This is done by first increasing the influence area by the allowed movement distance, and merging them with other influence areas if possible
    for (int layer_idx = int(move_bounds.size()) - 1; layer_idx > 0; -- layer_idx)
        if (SupportElements &prev_layer = move_bounds[layer_idx]; ! prev_layer.empty()) {
            // merging is expensive and only parallelized to a max speedup of 2. As such it may be useful in some cases to only merge every few layers to improve performance.
            bool had_new_element = new_element;
            const bool merge_this_layer = had_new_element || size_t(last_merge_layer_idx - layer_idx) >= merge_every_x_layers;
            if (had_new_element)
                merge_every_x_layers = 1;
            const auto ta               = std::chrono::high_resolution_clock::now();

            // ### Increase the influence areas by the allowed movement distance
            std::vector<SupportElementMerging> influence_areas;
            influence_areas.reserve(prev_layer.size());
            for (int32_t element_idx = 0; element_idx < int32_t(prev_layer.size()); ++ element_idx) {
                SupportElement &el = prev_layer[element_idx];
                assert(!el.influence_area.empty());
                SupportElement::ParentIndices parents;
                parents.emplace_back(element_idx);
                influence_areas.push_back({ el.state, parents });
            }
            increase_areas_one_layer(volumes, config, influence_areas, layer_idx, prev_layer, merge_this_layer, throw_on_cancel);

            // Place already fully constructed elements to the output, remove them from influence_areas.
            SupportElements &this_layer = move_bounds[layer_idx - 1];
            influence_areas.erase(std::remove_if(influence_areas.begin(), influence_areas.end(),
                [&this_layer, &_tiny_area_threshold, layer_idx](SupportElementMerging &elem) {
                    if (elem.areas.influence_areas.empty())
                        // This area was removed completely due to collisions.
                        return true;
                    if (elem.areas.to_bp_areas.empty() && elem.areas.to_model_areas.empty()) {
                        if (area(elem.areas.influence_areas) < _tiny_area_threshold) {
                            BOOST_LOG_TRIVIAL(warning) << "Insert Error of Influence area bypass on layer " << layer_idx - 1;
                            tree_supports_show_error("Insert error of area after bypassing merge.\n"sv, true);
                        }
                        // Move the area to output.
                        this_layer.emplace_back(elem.state, std::move(elem.parents), std::move(elem.areas.influence_areas));
                        return true;
                    }
                    // Keep the area.
                    return false;
                }),
                influence_areas.end());

            dur_inc += std::chrono::high_resolution_clock::now() - ta;
            new_element = ! move_bounds[layer_idx - 1].empty();
            if (merge_this_layer) {
                bool reduced_by_merging = false;
                if (size_t count_before_merge = influence_areas.size(); count_before_merge > 1) {
                    // ### Calculate which influence areas overlap, and merge them into a new influence area (simplified: an intersection of influence areas that have such an intersection)
                    merge_influence_areas(volumes, config, layer_idx, influence_areas, throw_on_cancel);
                    reduced_by_merging = count_before_merge > influence_areas.size();
                }
                last_merge_layer_idx = layer_idx;
                if (! reduced_by_merging && ! had_new_element)
                    merge_every_x_layers = std::min(max_merge_every_x_layers, merge_every_x_layers + 1);
            }

            dur_total += std::chrono::high_resolution_clock::now() - ta;

            // Save calculated elements to output, and allocate Polygons on heap, as they will not be changed again.
            for (SupportElementMerging &elem : influence_areas)
                if (! elem.areas.influence_areas.empty()) {
                    Polygons new_area = safe_union(elem.areas.influence_areas);
                    if (area(new_area) < _tiny_area_threshold) {
                        BOOST_LOG_TRIVIAL(warning) << "Insert Error of Influence area on layer " << layer_idx - 1 << ". Origin of " << elem.parents.size() << " areas. Was to bp " << elem.state.to_buildplate;
                        tree_supports_show_error("Insert error of area after merge.\n"sv, true);
                    }
                    this_layer.emplace_back(elem.state, std::move(elem.parents), std::move(new_area));
                }

    #ifdef SLIC3R_TREESUPPORTS_PROGRESS
            progress_total += data_size_inverse * TREE_PROGRESS_AREA_CALC;
            Progress::messageProgress(Progress::Stage::SUPPORT, progress_total * m_progress_multiplier + m_progress_offset, TREE_PROGRESS_TOTAL);
    #endif
            throw_on_cancel();
        }

    BOOST_LOG_TRIVIAL(info) << "Time spent with creating influence areas' subtasks: Increasing areas " << dur_inc.count() / 1000000 <<
        " ms merging areas: " << (dur_total - dur_inc).count() / 1000000 << " ms";
}

/*!
 * \brief Sets the result_on_layer for all parents based on the SupportElement supplied.
 *
 * \param elem[in] The SupportElements, which parent's position should be determined.
 */
static Point choose_balanced_parent_point(
    const TreeSupportSettings &config,
    const Polygons            &influence_area,
    const Point               &preferred,
    const std::vector<Point>  &occupied)
{
    if (influence_area.empty())
        return preferred;
    const Point baseline = move_inside_if_outside(influence_area, preferred);
    if (occupied.empty())
        return baseline;

    const coord_t requested_spacing = std::max<coord_t>(config.settings.support_tree_branch_distance, scale_(0.5));
    const int64_t requested_spacing2 = int64_t(requested_spacing) * int64_t(requested_spacing);
    // Respect the original tree planner's per-layer movement limit. A larger
    // rebalancing jump would require a new output plane and can create a
    // disconnected trunk even when the influence areas overlap. The slow
    // movement allowance is still valid for a gradual branch, but a vertical
    // branch must remain vertical.
    const coord_t movement_limit = std::max(config.maximum_move_distance, config.maximum_move_distance_slow);
    const coord_t max_shift = std::min(requested_spacing, std::max<coord_t>(movement_limit, 0));
    const int64_t max_shift2 = int64_t(max_shift) * int64_t(max_shift);
    auto min_distance2 = [&occupied](const Point &candidate) {
        int64_t result = std::numeric_limits<int64_t>::max();
        for (const Point &point : occupied)
            result = std::min(result, int64_t((candidate - point).cast<int64_t>().squaredNorm()));
        return result;
    };
    const int64_t baseline_distance2 = min_distance2(baseline);
    if (baseline_distance2 >= requested_spacing2)
        return baseline;

    std::vector<Point> candidates{baseline};
    candidates.reserve(64);
    const BoundingBox bounds = get_extents(influence_area);
    auto add_candidate = [&](Point candidate) {
        if (candidates.size() >= 64)
            return;
        candidate = move_inside_if_outside(influence_area, candidate);
        if (std::none_of(candidates.begin(), candidates.end(), [&](const Point &other) { return candidate == other; }))
            candidates.push_back(candidate);
    };
    add_candidate(bounds.min);
    add_candidate(bounds.max);
    add_candidate(Point(bounds.min.x(), bounds.max.y()));
    add_candidate(Point(bounds.max.x(), bounds.min.y()));
    add_candidate(Point((bounds.min.x() + bounds.max.x()) / 2, (bounds.min.y() + bounds.max.y()) / 2));
    add_candidate(Point((bounds.min.x() + bounds.max.x()) / 2, bounds.min.y()));
    add_candidate(Point((bounds.min.x() + bounds.max.x()) / 2, bounds.max.y()));
    add_candidate(Point(bounds.min.x(), (bounds.min.y() + bounds.max.y()) / 2));
    add_candidate(Point(bounds.max.x(), (bounds.min.y() + bounds.max.y()) / 2));
    for (const Polygon &polygon : influence_area) {
        add_candidate(polygon.centroid());
        const size_t stride = std::max<size_t>(1, polygon.points.size() / 20);
        for (size_t i = 0; i < polygon.points.size(); i += stride) {
            add_candidate(polygon.points[i]);
            if (i + 1 < polygon.points.size())
                add_candidate(Point((polygon.points[i].x() + polygon.points[i + 1].x()) / 2,
                                    (polygon.points[i].y() + polygon.points[i + 1].y()) / 2));
        }
    }
    if (max_shift > 0) {
        constexpr double directions = 8.;
        for (int i = 0; i < int(directions); ++i) {
            const double angle = 2. * M_PI * double(i) / directions;
            add_candidate(Point(baseline.x() + coord_t(std::cos(angle) * double(max_shift)),
                                baseline.y() + coord_t(std::sin(angle) * double(max_shift))));
        }
    }

    Point best = baseline;
    int64_t best_distance2 = baseline_distance2;
    int64_t best_preferred_distance2 = 0;
    for (const Point &candidate : candidates) {
        const int64_t preferred_distance2 = int64_t((candidate - baseline).cast<int64_t>().squaredNorm());
        if (preferred_distance2 > max_shift2)
            continue;
        const int64_t distance2 = min_distance2(candidate);
        if (distance2 > best_distance2 || (distance2 == best_distance2 && preferred_distance2 < best_preferred_distance2)) {
            best = candidate;
            best_distance2 = distance2;
            best_preferred_distance2 = preferred_distance2;
        }
    }
    return best;
}

static void set_points_on_areas(const TreeSupportSettings &config, const SupportElement &elem,
    SupportElements *layer_above, std::vector<BoundingBox> &layer_above_bounds)
{
    assert(!elem.state.deleted);
    assert(layer_above != nullptr || elem.parents.empty());

    // Based on the branch center point of the current layer, the point on the next (further up) layer is calculated.
    if (! elem.state.result_on_layer_is_set()) {
        BOOST_LOG_TRIVIAL(warning) << "Uninitialized support element";
        tree_supports_show_error("Uninitialized support element. A branch may be missing.\n"sv, true);
        return;
    }

    if (layer_above) {
        // A single parent follows the original centerline. It cannot benefit
        // from sibling balancing, so avoid scanning the whole layer above on
        // this common path.
        if (elem.parents.size() <= 1) {
            for (int32_t next_elem_idx : elem.parents) {
                assert(next_elem_idx >= 0);
                SupportElement &next_elem = (*layer_above)[next_elem_idx];
                assert(!next_elem.state.deleted);
                if (!next_elem.state.result_on_layer_is_set())
                    next_elem.state.result_on_layer = move_inside_if_outside(
                        next_elem.influence_area, elem.state.result_on_layer);
                next_elem.state.marked = true;
            }
            return;
        }
        std::vector<Point> occupied;
        occupied.reserve(layer_above->size());
        // Only trunks belonging to the same support network should repel one
        // another.  A single global mask makes unrelated trees move away from
        // each other and changes otherwise valid paths.  Bounding boxes are a
        // deterministic, cheap connectivity guard; influence-area geometry
        // remains the authoritative collision constraint below.
        const coord_t network_margin = std::max<coord_t>(
            config.settings.support_tree_branch_distance, scale_(0.5));
        const BoundingBox network_bounds = get_extents(elem.influence_area).inflated(network_margin);
        if (layer_above_bounds.empty()) {
            layer_above_bounds.reserve(layer_above->size());
            for (const SupportElement &candidate : *layer_above)
                layer_above_bounds.push_back(get_extents(candidate.influence_area).inflated(network_margin));
        }
        for (size_t candidate_idx = 0; candidate_idx < layer_above->size(); ++candidate_idx) {
            const SupportElement &candidate = (*layer_above)[candidate_idx];
            if (!candidate.state.result_on_layer_is_set())
                continue;
            if (candidate_idx < layer_above_bounds.size() && network_bounds.overlap(layer_above_bounds[candidate_idx]))
                occupied.push_back(candidate.state.result_on_layer);
        }
        for (int32_t next_elem_idx : elem.parents) {
            assert(next_elem_idx >= 0);
            SupportElement &next_elem = (*layer_above)[next_elem_idx];
            assert(! next_elem.state.deleted);
            if (!next_elem.state.result_on_layer_is_set()) {
                next_elem.state.result_on_layer = choose_balanced_parent_point(
                    config, next_elem.influence_area, elem.state.result_on_layer, occupied);
                occupied.push_back(next_elem.state.result_on_layer);
            }
            next_elem.state.marked = true;
        }
    }
}

static void set_to_model_contact_simple(SupportElement &elem)
{
    const Point best = move_inside_if_outside(elem.influence_area, elem.state.next_position);
    elem.state.result_on_layer = best;
    BOOST_LOG_TRIVIAL(debug) << "Added NON gracious Support On Model Point (" << best.x() << "," << best.y() << "). The current layer is " << elem.state.layer_idx;
}

/*!
 * \brief Get the best point to connect to the model and set the result_on_layer of the relevant SupportElement accordingly.
 *
 * \param move_bounds[in,out] All currently existing influence areas
 * \param first_elem[in,out] SupportElement that did not have its result_on_layer set meaning that it does not have a child element.
 * \param layer_idx[in] The current layer.
 */
static void set_to_model_contact_to_model_gracious(
    const TreeModelVolumes          &volumes,
    const TreeSupportSettings       &config,
    std::vector<SupportElements>    &move_bounds,
    SupportElement                  &first_elem,
    std::function<void()>            throw_on_cancel)
{
    SupportElement *last_successfull_layer = nullptr;

    // check for every layer upwards, up to the point where this influence area was created (either by initial insert or merge) if the branch could be placed on it, and highest up layer index.
    {
        SupportElement *elem = &first_elem;
        for (LayerIndex layer_check = elem->state.layer_idx;
            ! intersection(elem->influence_area, volumes.getPlaceableAreas(support_element_collision_radius(config, elem->state), layer_check, throw_on_cancel)).empty();
            elem = &move_bounds[++ layer_check][elem->parents.front()]) {
            assert(elem->state.layer_idx == layer_check);
            assert(! elem->state.deleted);
            assert(elem->state.to_model_gracious);
            last_successfull_layer = elem;
            if (elem->parents.size() != 1)
                // Reached merge point.
                break;
        }
    }

    // Could not find valid placement, even though it should exist => error handling
    if (last_successfull_layer == nullptr) {
        BOOST_LOG_TRIVIAL(warning) << "No valid placement found for to model gracious element on layer " << first_elem.state.layer_idx;
        tree_supports_show_error("Could not fine valid placement on model! Just placing it down anyway. Could cause floating branches."sv, true);
        first_elem.state.to_model_gracious = false;
        set_to_model_contact_simple(first_elem);
    } else {
        // Found a gracious area above first_elem. Remove all below last_successfull_layer.
        {
            LayerIndex parent_layer_idx = first_elem.state.layer_idx;
            for (SupportElement *elem = &first_elem; elem != last_successfull_layer; elem = &move_bounds[++ parent_layer_idx][elem->parents.front()]) {
                assert(! elem->state.deleted);
                elem->state.deleted = true;
            }
        }
        // Guess a point inside the influence area, in which the branch will be placed in.
        const Point best = move_inside_if_outside(last_successfull_layer->influence_area, last_successfull_layer->state.next_position);
        last_successfull_layer->state.result_on_layer = best;
        BOOST_LOG_TRIVIAL(debug) << "Added gracious Support On Model Point (" << best.x() << "," << best.y() << "). The current layer is " << last_successfull_layer;
    }
}

// Remove elements marked as "deleted", update indices to parents.
static void remove_deleted_elements(std::vector<SupportElements> &move_bounds)
{
    std::vector<int32_t> map_parents;
    std::vector<int32_t> map_current;
    for (LayerIndex layer_idx = LayerIndex(move_bounds.size()) - 1; layer_idx >= 0; -- layer_idx) {
        SupportElements &layer = move_bounds[layer_idx];
        map_current.clear();
        for (int32_t i = 0; i < int32_t(layer.size());) {
            SupportElement &element = layer[i];
            if (element.state.deleted) {
                if (map_current.empty()) {
                    // Initialize with identity map.
                    map_current.assign(layer.size(), 0);
                    std::iota(map_current.begin(), map_current.end(), 0);
                }
                // Delete all "deleted" elements from the end of the layer vector.
                while (i < int32_t(layer.size()) && layer.back().state.deleted) {
                    layer.pop_back();
                    // Mark as deleted in the map.
                    map_current[layer.size()] = -1;
                }
                assert(i == layer.size() || i + 1 < layer.size());
                if (i + 1 < int32_t(layer.size())) {
                    element = std::move(layer.back());
                    layer.pop_back();
                    // Mark the current element as deleted.
                    map_current[i] = -1;
                    // Mark the moved element as moved to index i.
                    map_current[layer.size()] = i;
                }
            } else {
                // Current element is not deleted. Update its parent indices.
                if (! map_parents.empty())
                    for (int32_t &parent_idx : element.parents)
                        parent_idx = map_parents[parent_idx];
                ++ i;
            }
        }
        std::swap(map_current, map_parents);
    }
}

/*!
 * \brief Set the result_on_layer point for all influence areas
 *
 * \param move_bounds[in,out] All currently existing influence areas
 */
static void create_nodes_from_area(
    const TreeModelVolumes       &volumes,
    const TreeSupportSettings    &config,
    std::vector<SupportElements> &move_bounds,
    std::function<void()>         throw_on_cancel)
{
    // Initialize points on layer 0, with a "random" point in the influence area.
    // Point is chosen based on an inaccurate estimate where the branches will split into two, but every point inside the influence area would produce a valid result.
    {
        SupportElements *layer_above = move_bounds.size() > 1 ? &move_bounds[1] : nullptr;
        std::vector<BoundingBox> layer_above_bounds;
        if (layer_above) {
            for (SupportElement &elem : *layer_above)
                elem.state.marked = false;
        }
        for (SupportElement &init : move_bounds.front()) {
            init.state.result_on_layer = move_inside_if_outside(init.influence_area, init.state.next_position);
            // Also set the parent nodes, as these will be required for the first iteration of the loop below and mark the parent nodes.
            set_points_on_areas(config, init, layer_above, layer_above_bounds);
        }
    }

    throw_on_cancel();

    for (LayerIndex layer_idx = 1; layer_idx < LayerIndex(move_bounds.size()); ++ layer_idx) {
        auto &layer       = move_bounds[layer_idx];
        auto *layer_above = layer_idx + 1 < LayerIndex(move_bounds.size()) ? &move_bounds[layer_idx + 1] : nullptr;
        std::vector<BoundingBox> layer_above_bounds;
        if (layer_above)
            for (SupportElement &elem : *layer_above)
                elem.state.marked = false;
        for (SupportElement &elem : layer) {
            assert(! elem.state.deleted);
            assert(elem.state.layer_idx == layer_idx);
            // check if the resulting center point is not yet set
            if (! elem.state.result_on_layer_is_set()) {
                if (elem.state.to_buildplate || (elem.state.distance_to_top < config.min_dtt_to_model && ! elem.state.supports_roof)) {
                    if (elem.state.to_buildplate) {
                        BOOST_LOG_TRIVIAL(warning) << "Uninitialized Influence area targeting " << elem.state.target_position.x() << "," << elem.state.target_position.y()
                                                   << ") "
                            "at target_height: " << elem.state.target_height << " layer: " << layer_idx;
                        tree_supports_show_error("Uninitialized support element! A branch could be missing or exist partially."sv, true);
                    }
                    // we dont need to remove yet the parents as they will have a lower dtt and also no result_on_layer set
                    elem.state.deleted = true;
                } else {
                    // set the point where the branch will be placed on the model
                    if (elem.state.to_model_gracious)
                        set_to_model_contact_to_model_gracious(volumes, config, move_bounds, elem, throw_on_cancel);
                    else
                        set_to_model_contact_simple(elem);
                }
            }
            if (! elem.state.deleted && ! elem.state.marked && elem.state.target_height == layer_idx)
                // Just a tip surface with no supporting element.
                elem.state.deleted = true;
            if (elem.state.deleted) {
                for (int32_t parent_idx : elem.parents)
                    // When the roof was not able to generate downwards enough, the top elements may have not moved, and have result_on_layer already set.
                    // As this branch needs to be removed => all parents result_on_layer have to be invalidated.
                    (*layer_above)[parent_idx].state.result_on_layer_reset();
            }
            if (! elem.state.deleted) {
                // Element is valid now setting points in the layer above and mark the parent nodes.
                set_points_on_areas(config, elem, layer_above, layer_above_bounds);
            }
        }
        throw_on_cancel();
    }

#ifndef NDEBUG
    // Verify the tree connectivity including the branch slopes.
    for (LayerIndex layer_idx = 0; layer_idx + 1 < LayerIndex(move_bounds.size()); ++ layer_idx) {
        auto &layer = move_bounds[layer_idx];
        auto &above = move_bounds[layer_idx + 1];
        for (SupportElement &elem : layer)
            if (! elem.state.deleted) {
                for (int32_t iparent : elem.parents) {
                    SupportElement &parent = above[iparent];
                    assert(! parent.state.deleted);
                    assert(elem.state.result_on_layer_is_set() == parent.state.result_on_layer_is_set());
                    if (elem.state.result_on_layer_is_set()) {
                        double radius_increase = support_element_radius(config, elem) - support_element_radius(config, parent);
                        assert(radius_increase >= 0);
                        double shift = (elem.state.result_on_layer - parent.state.result_on_layer).cast<double>().norm();
                        //FIXME this assert fails a lot. Is it correct?
//                        assert(shift < radius_increase + 2. * config.maximum_move_distance_slow);
                    }
                }
            }
        }
#endif // NDEBUG

    remove_deleted_elements(move_bounds);

#ifndef NDEBUG
    // Verify the tree connectivity including the branch slopes.
    for (LayerIndex layer_idx = 0; layer_idx + 1 < LayerIndex(move_bounds.size()); ++ layer_idx) {
        auto &layer = move_bounds[layer_idx];
        auto &above = move_bounds[layer_idx + 1];
        for (SupportElement &elem : layer) {
            assert(! elem.state.deleted);
            for (int32_t iparent : elem.parents) {
                SupportElement &parent = above[iparent];
                assert(! parent.state.deleted);
                assert(elem.state.result_on_layer_is_set() == parent.state.result_on_layer_is_set());
                if (elem.state.result_on_layer_is_set()) {
                    double radius_increase = support_element_radius(config, elem) - support_element_radius(config, parent);
                    assert(radius_increase >= 0);
                    double shift = (elem.state.result_on_layer - parent.state.result_on_layer).cast<double>().norm();
                    //FIXME this assert fails a lot. Is it correct?
//                    assert(shift < radius_increase + 2. * config.maximum_move_distance_slow);
                }
            }
        }
    }
#endif // NDEBUG
}

template<bool flip_normals>
void triangulate_fan(indexed_triangle_set &its, int ifan, int ibegin, int iend)
{
    // at least 3 vertices, increasing order.
    assert(ibegin + 3 <= iend);
    assert(ibegin >= 0 && iend <= its.vertices.size());
    assert(ifan >= 0 && ifan < its.vertices.size());
    int num_faces = iend - ibegin;
    its.indices.reserve(its.indices.size() + num_faces * 3);
    for (int v = ibegin, u = iend - 1; v < iend; u = v ++) {
        if (flip_normals)
            its.indices.push_back({ ifan, u, v });
        else
            its.indices.push_back({ ifan, v, u });
    }
}

static void triangulate_strip(indexed_triangle_set &its, int ibegin1, int iend1, int ibegin2, int iend2)
{
    // at least 3 vertices, increasing order.
    assert(ibegin1 + 3 <= iend1);
    assert(ibegin1 >= 0 && iend1 <= its.vertices.size());
    assert(ibegin2 + 3 <= iend2);
    assert(ibegin2 >= 0 && iend2 <= its.vertices.size());
    int n1 = iend1 - ibegin1;
    int n2 = iend2 - ibegin2;
    its.indices.reserve(its.indices.size() + (n1 + n2) * 3);

    // For the first vertex of 1st strip, find the closest vertex on the 2nd strip.
    int istart2 = ibegin2;
    {
        const Vec3f &p1    = its.vertices[ibegin1];
        auto         d2min = std::numeric_limits<float>::max();
        for (int i = ibegin2; i < iend2; ++ i) {
            const Vec3f &p2 = its.vertices[i];
            const float d2  = (p2 - p1).squaredNorm();
            if (d2 < d2min) {
                d2min = d2;
                istart2 = i;
            }
        }
    }

    // Now triangulate the strip zig-zag fashion taking always the shortest connection if possible.
    for (int u = ibegin1, v = istart2; n1 > 0 || n2 > 0;) {
        bool take_first;
        int u2, v2;
        auto update_u2 = [&u2, u, ibegin1, iend1]() {
            u2 = u;
            if (++ u2 == iend1)
                u2 = ibegin1;
        };
        auto update_v2 = [&v2, v, ibegin2, iend2]() {
            v2 = v;
            if (++ v2 == iend2)
                v2 = ibegin2;
        };
        if (n1 == 0) {
            take_first = false;
            update_v2();
        } else if (n2 == 0) {
            take_first = true;
            update_u2();
        } else {
            update_u2();
            update_v2();
            float l1 = (its.vertices[u2] - its.vertices[v]).squaredNorm();
            float l2 = (its.vertices[v2] - its.vertices[u]).squaredNorm();
            take_first = l1 < l2;
        }
        if (take_first) {
            its.indices.push_back({ u, u2, v });
            -- n1;
            u = u2;
        } else {
            its.indices.push_back({ u, v2, v });
            -- n2;
            v = v2;
        }
    }
}

// Discretize 3D circle, append to output vector, return ranges of indices of the points added.
static std::pair<int, int> discretize_circle(const Vec3f &center, const Vec3f &normal, const float radius, const float eps, std::vector<Vec3f> &pts)
{
    // Calculate discretization step and number of steps.
    float angle_step = 2. * acos(1. - eps / radius);
    auto  nsteps     = int(ceil(2 * M_PI / angle_step));
    angle_step = 2 * M_PI / nsteps;

    // Prepare coordinate system for the circle plane.
    Vec3f x = normal.cross(Vec3f(0.f, -1.f, 0.f)).normalized();
    Vec3f y = normal.cross(x).normalized();
    assert(std::abs(x.cross(y).dot(normal) - 1.f) < EPSILON);

    // Discretize the circle.
    int begin = int(pts.size());
    pts.reserve(pts.size() + nsteps);
    float angle = 0;
    x *= radius;
    y *= radius;
    for (int i = 0; i < nsteps; ++ i) {
        pts.emplace_back(center + x * cos(angle) + y * sin(angle));
        angle += angle_step;
    }
    return { begin, int(pts.size()) };
}

// Returns Z span of the generated mesh.
static std::pair<float, float> extrude_branch(
    const std::vector<const SupportElement*>&path,
    const TreeSupportSettings               &config,
    const SlicingParameters                 &slicing_params,
    const std::vector<SupportElements>      &move_bounds,
    bool                                     has_root,
    indexed_triangle_set                    &result)
{
    Vec3d p1, p2, p3;
    Vec3d v1, v2;
    Vec3d nprev;
    Vec3d ncurrent;
    assert(path.size() >= 2);
    static constexpr const float eps = 0.015f;
    std::pair<int, int> prev_strip;
    float zmin = 0;
    float zmax = 0;

    for (size_t ipath = 1; ipath < path.size(); ++ ipath) {
        const SupportElement &prev    = *path[ipath - 1];
        const SupportElement &current = *path[ipath];
        assert(prev.state.layer_idx + 1 == current.state.layer_idx);
        p1 = to_3d(unscaled<double>(prev   .state.result_on_layer), layer_z(slicing_params, config, prev   .state.layer_idx));
        p2 = to_3d(unscaled<double>(current.state.result_on_layer), layer_z(slicing_params, config, current.state.layer_idx));
        v1 = (p2 - p1).normalized();
        if (ipath == 1) {
            nprev = v1;
            float radius     = unscaled<float>(support_element_radius(config, prev));
            if (has_root && prev.state.layer_idx == 0) {
                // Inlong: Buildplate roots need a flat foot. A rounded cap can extend far
                // below the bed and make the first layer slice cut unrelated trunk geometry.
                const Vec3f normal(0.f, 0.f, 1.f);
                const Vec3f bottom_center(float(p1.x()), float(p1.y()), 0.f);
                const Vec3f top_center(float(p1.x()), float(p1.y()), float(p1.z()));
                int ifan = int(result.vertices.size());
                result.vertices.emplace_back(bottom_center);
                std::pair<int, int> bottom_strip = discretize_circle(bottom_center, normal, radius, eps, result.vertices);
                triangulate_fan<false>(result, ifan, bottom_strip.first, bottom_strip.second);
                prev_strip = discretize_circle(top_center, normal, radius, eps, result.vertices);
                triangulate_strip(result, bottom_strip.first, bottom_strip.second, prev_strip.first, prev_strip.second);
                zmin = 0.f;
            } else {
                // Extrude the bottom half sphere.
                float angle_step = 2. * acos(1. - eps / radius);
                auto  nsteps     = int(ceil(M_PI / (2. * angle_step)));
                angle_step       = M_PI / (2. * nsteps);
                int   ifan       = int(result.vertices.size());
                result.vertices.emplace_back((p1 - nprev * radius).cast<float>());
                zmin = result.vertices.back().z();
                float angle = angle_step;
                for (int i = 1; i < nsteps; ++ i, angle += angle_step) {
                    std::pair<int, int> strip = discretize_circle((p1 - nprev * radius * cos(angle)).cast<float>(), nprev.cast<float>(), radius * sin(angle), eps, result.vertices);
                    if (i == 1)
                        triangulate_fan<false>(result, ifan, strip.first, strip.second);
                    else
                        triangulate_strip(result, prev_strip.first, prev_strip.second, strip.first, strip.second);

                    prev_strip = strip;
                }
            }
        }
        if (ipath + 1 == path.size()) {
            // End of the tube.
            ncurrent = v1;
            // Extrude the top half sphere.
            float radius = unscaled<float>(support_element_radius(config, current));
            float angle_step = 2. * acos(1. - eps / radius);
            auto  nsteps = int(ceil(M_PI / (2. * angle_step)));
            angle_step = M_PI / (2. * nsteps);
            auto angle = float(M_PI / 2.);
            for (int i = 0; i < nsteps; ++ i, angle -= angle_step) {
                std::pair<int, int> strip = discretize_circle((p2 + ncurrent * radius * cos(angle)).cast<float>(), ncurrent.cast<float>(), radius * sin(angle), eps, result.vertices);
                triangulate_strip(result, prev_strip.first, prev_strip.second, strip.first, strip.second);
//                sprintf(fname, "d:\\temp\\meshes\\tree-partial-%d.obj", ++ irun);
//                its_write_obj(result, fname);
                prev_strip = strip;
            }
            int ifan = int(result.vertices.size());
            result.vertices.emplace_back((p2 + ncurrent * radius).cast<float>());
            zmax = result.vertices.back().z();
            triangulate_fan<true>(result, ifan, prev_strip.first, prev_strip.second);
//            sprintf(fname, "d:\\temp\\meshes\\tree-partial-%d.obj", ++ irun);
//            its_write_obj(result, fname);
        } else {
            const SupportElement &next = *path[ipath + 1];
            assert(current.state.layer_idx + 1 == next.state.layer_idx);
            p3 = to_3d(unscaled<double>(next.state.result_on_layer), layer_z(slicing_params, config, next.state.layer_idx));
            v2 = (p3 - p2).normalized();
            ncurrent = (v1 + v2).normalized();
            float radius = unscaled<float>(support_element_radius(config, current));
            std::pair<int, int> strip = discretize_circle(p2.cast<float>(), ncurrent.cast<float>(), radius, eps, result.vertices);
            triangulate_strip(result, prev_strip.first, prev_strip.second, strip.first, strip.second);
            prev_strip = strip;
//            sprintf(fname, "d:\\temp\\meshes\\tree-partial-%d.obj", ++irun);
//            its_write_obj(result, fname);
        }
#if 0
        if (circles_intersect(p1, nprev, support_element_radius(settings, prev), p2, ncurrent, support_element_radius(settings, current))) {
            // Cannot connect previous and current slice using a simple zig-zag triangulation,
            // because the two circles intersect.

        } else {
            // Continue with chaining.

        }
#endif
    }

    return std::make_pair(zmin, zmax);
}


#ifdef TREE_SUPPORT_ORGANIC_NUDGE_NEW

// New version using per layer AABB trees of lines for nudging spheres away from an object.
static void organic_smooth_branches_avoid_collisions(
    const PrintObject                                   &print_object,
    const TreeModelVolumes                              &volumes,
    const TreeSupportSettings                           &config,
    std::vector<SupportElements>                        &move_bounds,
    const std::vector<std::pair<SupportElement*, int>>  &elements_with_link_down,
    const std::vector<size_t>                           &linear_data_layers,
    std::function<void()>                                throw_on_cancel)
{
    struct LayerCollisionCache {
        coord_t          min_element_radius{ std::numeric_limits<coord_t>::max() };
        bool             min_element_radius_known() const { return this->min_element_radius != std::numeric_limits<coord_t>::max(); }
        coord_t          collision_radius{ 0 };
        std::vector<Linef> lines;
        AABBTreeIndirect::Tree<2, double> aabbtree_lines;
        bool             empty() const { return this->lines.empty(); }
    };
    std::vector<LayerCollisionCache> layer_collision_cache;
    layer_collision_cache.reserve(1024);
    const SlicingParameters &slicing_params = print_object.slicing_parameters();
    for (const std::pair<SupportElement*, int>& element : elements_with_link_down) {
        LayerIndex layer_idx = element.first->state.layer_idx;
        if (size_t num_layers = layer_idx + 1; num_layers > layer_collision_cache.size()) {
            if (num_layers > layer_collision_cache.capacity())
                reserve_power_of_2(layer_collision_cache, num_layers);
            layer_collision_cache.resize(num_layers, {});
        }
        auto& l = layer_collision_cache[layer_idx];
        l.min_element_radius = std::min(l.min_element_radius, support_element_radius(config, *element.first));
    }

    throw_on_cancel();

    for (LayerIndex layer_idx = 0; layer_idx < LayerIndex(layer_collision_cache.size()); ++layer_idx)
        if (LayerCollisionCache& l = layer_collision_cache[layer_idx]; !l.min_element_radius_known())
            l.min_element_radius = 0;
        else {
            //FIXME
            l.min_element_radius = 0;
            std::optional<std::pair<coord_t, std::reference_wrapper<const Polygons>>> res = volumes.get_collision_lower_bound_area(layer_idx, l.min_element_radius);
            assert(res.has_value());
            l.collision_radius = res->first;
            Lines alines = to_lines(res->second.get());
            l.lines.reserve(alines.size());
            for (const Line &line : alines)
                l.lines.push_back({ unscaled<double>(line.a), unscaled<double>(line.b) });
            l.aabbtree_lines = AABBTreeLines::build_aabb_tree_over_indexed_lines(l.lines);
            throw_on_cancel();
        }

    struct CollisionSphere {
        const SupportElement& element;
        int                   element_below_id;
        const bool            locked;
        float                 radius;
        // Current position, when nudged away from the collision.
        Vec3f                 position;
        // Previous position, for Laplacian smoothing.
        Vec3f                 prev_position;
        //
        Vec3f                 last_collision;
        double                last_collision_depth;
        // Minimum Z for which the sphere collision will be evaluated.
        // Limited by the minimum sloping angle and by the bottom of the tree.
        float                 min_z{ -std::numeric_limits<float>::max() };
        // Maximum Z for which the sphere collision will be evaluated.
        // Limited by the minimum sloping angle and by the tip of the current branch.
        float                 max_z{ std::numeric_limits<float>::max() };
        uint32_t              layer_begin;
        uint32_t              layer_end;
    };

    std::vector<CollisionSphere> collision_spheres;
    collision_spheres.reserve(elements_with_link_down.size());
    for (const std::pair<SupportElement*, int> &element_with_link : elements_with_link_down) {
        const SupportElement &element   = *element_with_link.first;
        const int             link_down = element_with_link.second;
        collision_spheres.push_back({
            element,
            link_down,
            // locked
            element.parents.empty() || (link_down == -1 && element.state.layer_idx > 0),
            unscaled<float>(support_element_radius(config, element)),
            // 3D position
            to_3d(unscaled<float>(element.state.result_on_layer), float(layer_z(slicing_params, config, element.state.layer_idx)))
        });
        // Update min_z coordinate to min_z of the tree below.
        CollisionSphere &collision_sphere = collision_spheres.back();
        if (link_down != -1) {
            const size_t offset_below = linear_data_layers[element.state.layer_idx - 1];
            collision_sphere.min_z = collision_spheres[offset_below + link_down].min_z;
        } else
            collision_sphere.min_z = collision_sphere.position.z();
    }
    // Update max_z by propagating max_z from the tips of the branches.
    for (int collision_sphere_id = int(collision_spheres.size()) - 1; collision_sphere_id >= 0; -- collision_sphere_id) {
        CollisionSphere &collision_sphere = collision_spheres[collision_sphere_id];
        if (collision_sphere.element.parents.empty())
            // Tip
            collision_sphere.max_z = collision_sphere.position.z();
        else {
            // Below tip
            const size_t offset_above = linear_data_layers[collision_sphere.element.state.layer_idx + 1];
            for (auto iparent : collision_sphere.element.parents) {
                float parent_z = collision_spheres[offset_above + iparent].max_z;
//                    collision_sphere.max_z = collision_sphere.max_z == std::numeric_limits<float>::max() ? parent_z : std::max(collision_sphere.max_z, parent_z);
                collision_sphere.max_z = std::min(collision_sphere.max_z, parent_z);
            }
        }
    }
    // Update min_z / max_z to limit the search Z span of a given sphere for collision detection.
    for (CollisionSphere &collision_sphere : collision_spheres) {
        //FIXME limit the collision span by the tree slope.
        collision_sphere.min_z = std::max(collision_sphere.min_z, collision_sphere.position.z() - collision_sphere.radius);
        collision_sphere.max_z = std::min(collision_sphere.max_z, collision_sphere.position.z() + collision_sphere.radius);
        collision_sphere.layer_begin = std::min(collision_sphere.element.state.layer_idx, layer_idx_ceil(slicing_params, config, collision_sphere.min_z));
        assert(collision_sphere.layer_begin < layer_collision_cache.size());
        collision_sphere.layer_end   = std::min(LayerIndex(layer_collision_cache.size()), std::max(collision_sphere.element.state.layer_idx, layer_idx_floor(slicing_params, config, collision_sphere.max_z)) + 1);
    }

    throw_on_cancel();

    static constexpr const double collision_extra_gap = 0.1;
    static constexpr const double max_nudge_collision_avoidance = 0.5;
    static constexpr const double max_nudge_smoothing = 0.2;
    static constexpr const size_t num_iter = 100; // 1000;

    // Inlong:
    // Collision and Laplacian smoothing run iteratively; keep each candidate reachable from linked upper/lower layers to avoid accumulated drift.
    auto limit_candidate_to_linked_layers = [&collision_spheres, &linear_data_layers, &config](const size_t collision_sphere_id, Vec2d candidate) {
        auto constrain_to_anchor = [](Vec2d candidate, const Vec2d &current_pos, const Vec2d &anchor, double allowed_shift) {
            const Vec2d delta = candidate - anchor;
            const double candidate_dist = delta.norm();
            const double current_dist   = (current_pos - anchor).norm();
            allowed_shift = std::max(allowed_shift, current_dist);
            return candidate_dist > allowed_shift && candidate_dist > EPSILON ?
                anchor + delta * (allowed_shift / candidate_dist) :
                candidate;
        };

        const CollisionSphere &sphere = collision_spheres[collision_sphere_id];
        const LayerIndex layer_idx = sphere.element.state.layer_idx;
        const Vec2d current_pos = to_2d(sphere.position).cast<double>();
        const double current_radius = double(support_element_radius(config, sphere.element));
        const double maximum_move_distance_slow = double(config.maximum_move_distance_slow);

        if (sphere.element_below_id != -1 && layer_idx > 0) {
            const size_t lower_id = linear_data_layers[layer_idx - 1] + size_t(sphere.element_below_id);
            if (lower_id < collision_spheres.size()) {
                const CollisionSphere &lower = collision_spheres[lower_id];
                const double lower_radius = double(support_element_radius(config, lower.element));
                const double allowed_shift = unscaled<double>(std::max(0., lower_radius - current_radius) + maximum_move_distance_slow);
                candidate = constrain_to_anchor(candidate, current_pos, to_2d(lower.prev_position).cast<double>(), allowed_shift);
            }
        }

        const LayerIndex upper_layer_idx = layer_idx + 1;
        if (!sphere.element.parents.empty() && upper_layer_idx < LayerIndex(linear_data_layers.size())) {
            const size_t upper_offset = linear_data_layers[upper_layer_idx];
            for (int32_t parent_idx : sphere.element.parents) {
                const size_t upper_id = upper_offset + size_t(parent_idx);
                if (upper_id >= collision_spheres.size())
                    continue;
                const CollisionSphere &upper = collision_spheres[upper_id];
                const double upper_radius = double(support_element_radius(config, upper.element));
                const double allowed_shift = unscaled<double>(std::max(0., current_radius - upper_radius) + maximum_move_distance_slow);
                candidate = constrain_to_anchor(candidate, current_pos, to_2d(upper.prev_position).cast<double>(), allowed_shift);
            }
        }

        return candidate;
    };

    for (size_t iter = 0; iter < num_iter; ++ iter) {
        // Back up prev position before Laplacian smoothing.
        for (CollisionSphere &collision_sphere : collision_spheres)
            collision_sphere.prev_position = collision_sphere.position;
        std::atomic<size_t> num_moved{ 0 };
        tbb::parallel_for(tbb::blocked_range<size_t>(0, collision_spheres.size()),
            [&collision_spheres, &layer_collision_cache, &slicing_params, &config, &linear_data_layers, &num_moved, &throw_on_cancel, &limit_candidate_to_linked_layers](const tbb::blocked_range<size_t> range) {
            for (size_t collision_sphere_id = range.begin(); collision_sphere_id < range.end(); ++ collision_sphere_id)
                if (CollisionSphere &collision_sphere = collision_spheres[collision_sphere_id]; ! collision_sphere.locked) {
                    // Calculate collision of multiple 2D layers against a collision sphere.
                    collision_sphere.last_collision_depth = - std::numeric_limits<double>::max();
                    for (uint32_t layer_id = collision_sphere.layer_begin; layer_id != collision_sphere.layer_end; ++ layer_id) {
                        double dz = (layer_id - collision_sphere.element.state.layer_idx) * slicing_params.layer_height;
                        if (double r2 = sqr(collision_sphere.radius) - sqr(dz); r2 > 0) {
                            if (const LayerCollisionCache &layer_collision_cache_item = layer_collision_cache[layer_id]; ! layer_collision_cache_item.empty()) {
                                size_t hit_idx_out;
                                Vec2d  hit_point_out;
                                if (double dist = sqrt(AABBTreeLines::squared_distance_to_indexed_lines(
                                    layer_collision_cache_item.lines, layer_collision_cache_item.aabbtree_lines, Vec2d(to_2d(collision_sphere.position).cast<double>()),
                                    hit_idx_out, hit_point_out, r2)); dist >= 0.) {
                                    double collision_depth = sqrt(r2) - dist;
                                    if (collision_depth > collision_sphere.last_collision_depth) {
                                        collision_sphere.last_collision_depth = collision_depth;
                                        collision_sphere.last_collision = to_3d(hit_point_out.cast<float>(), float(layer_z(slicing_params, config, layer_id)));
                                    }
                                }
                            }
                        }
                    }
                    if (collision_sphere.last_collision_depth > 0) {
                        // Collision detected to be removed.
                        // Nudge the circle center away from the collision.
                        if (collision_sphere.last_collision_depth > EPSILON)
                            // a little bit of hysteresis to detect end of
                            ++ num_moved;
                        // Limit collision-avoidance nudge per iteration.
                        double nudge_dist = std::min(std::max(0., collision_sphere.last_collision_depth + collision_extra_gap), max_nudge_collision_avoidance);
                        Vec2d nudge_vector = (to_2d(collision_sphere.position) - to_2d(collision_sphere.last_collision)).cast<double>().normalized() * nudge_dist;
                        Vec2d candidate = to_2d(collision_sphere.position).cast<double>() + nudge_vector * nudge_dist;
                        candidate = limit_candidate_to_linked_layers(collision_sphere_id, candidate);
                        collision_sphere.position.head<2>() = candidate.cast<float>();
                    }
                    // Laplacian smoothing
                    Vec2d avg{ 0, 0 };
                    //const SupportElements &above = move_bounds[collision_sphere.element.state.layer_idx + 1];
                    const size_t           offset_above = linear_data_layers[collision_sphere.element.state.layer_idx + 1];
                    double weight = 0.;
                    for (auto iparent : collision_sphere.element.parents) {
                        double w = collision_sphere.radius;
                        avg += w * to_2d(collision_spheres[offset_above + iparent].prev_position.cast<double>());
                        weight += w;
                    }
                    if (collision_sphere.element_below_id != -1) {
                        const size_t offset_below = linear_data_layers[collision_sphere.element.state.layer_idx - 1];
                        const double w = weight; //  support_element_radius(config, move_bounds[element.state.layer_idx - 1][below]);
                        avg += w * to_2d(collision_spheres[offset_below + collision_sphere.element_below_id].prev_position.cast<double>());
                        weight += w;
                    }
                    avg /= weight;
                    static constexpr const double smoothing_factor = 0.5;
                    Vec2d old_pos = to_2d(collision_sphere.position).cast<double>();
                    Vec2d new_pos = (1. - smoothing_factor) * old_pos + smoothing_factor * avg;
                    Vec2d shift   = new_pos - old_pos;
                    double nudge_dist_max = shift.norm();
                    // Limit Laplacian smoothing nudge per iteration.
                    double nudge_dist = std::min(std::max(0., nudge_dist_max), max_nudge_smoothing);
                    if (nudge_dist > 0.) {
                        Vec2d candidate = old_pos + shift * (nudge_dist / nudge_dist_max);
                        candidate = limit_candidate_to_linked_layers(collision_sphere_id, candidate);
                        collision_sphere.position.head<2>() = candidate.cast<float>();
                    }

                    throw_on_cancel();
                }
        });
#if 0
        std::vector<double> stat;
        for (CollisionSphere& collision_sphere : collision_spheres)
            if (!collision_sphere.locked)
                stat.emplace_back(collision_sphere.last_collision_depth);
        std::sort(stat.begin(), stat.end());
        printf("iteration: %d, moved: %d, collision depth: min %lf, max %lf, median %lf\n", int(iter), int(num_moved), stat.front(), stat.back(), stat[stat.size() / 2]);
#endif
        if (num_moved == 0)
            break;
    }

    for (size_t i = 0; i < collision_spheres.size(); ++ i)
        elements_with_link_down[i].first->state.result_on_layer = scaled<coord_t>(to_2d(collision_spheres[i].position));
}
#else // TREE_SUPPORT_ORGANIC_NUDGE_NEW
// Old version using OpenVDB, works but it is extremely slow for complex meshes.
static void organic_smooth_branches_avoid_collisions(
    const PrintObject                                   &print_object,
    const TreeModelVolumes                              &volumes,
    const TreeSupportSettings                           &config,
    std::vector<SupportElements>                        &move_bounds,
    const std::vector<std::pair<SupportElement*, int>>  &elements_with_link_down,
    const std::vector<size_t>                           &linear_data_layers,
    std::function<void()>                                throw_on_cancel)
{
    TriangleMesh mesh = print_object.model_object()->raw_mesh();
    mesh.transform(print_object.trafo_centered());
    double scale = 10.;
    openvdb::FloatGrid::Ptr grid = mesh_to_grid(mesh.its, openvdb::math::Transform{}, scale, 0., 0.);
    std::unique_ptr<openvdb::tools::ClosestSurfacePoint<openvdb::FloatGrid>> closest_surface_point = openvdb::tools::ClosestSurfacePoint<openvdb::FloatGrid>::create(*grid);
    std::vector<openvdb::Vec3R> pts, prev, projections;
    std::vector<float> distances;
    for (const std::pair<SupportElement*, int>& element : elements_with_link_down) {
        Vec3d pt = to_3d(unscaled<double>(element.first->state.result_on_layer), layer_z(print_object.slicing_parameters(), config, element.first->state.layer_idx)) * scale;
        pts.push_back({ pt.x(), pt.y(), pt.z() });
    }

    const double collision_extra_gap = 1. * scale;
    const double max_nudge_collision_avoidance = 2. * scale;
    const double max_nudge_smoothing = 1. * scale;

    static constexpr const size_t num_iter = 100; // 1000;
    for (size_t iter = 0; iter < num_iter; ++ iter) {
        prev = pts;
        projections = pts;
        distances.assign(pts.size(), std::numeric_limits<float>::max());
        closest_surface_point->searchAndReplace(projections, distances);
        size_t num_moved = 0;
        for (size_t i = 0; i < projections.size(); ++ i) {
            const SupportElement &element = *elements_with_link_down[i].first;
            const int            below    = elements_with_link_down[i].second;
            const bool           locked   = (below == -1 && element.state.layer_idx > 0) || element.state.locked();
            if (! locked && pts[i] != projections[i]) {
                // Nudge the circle center away from the collision.
                Vec3d v{ projections[i].x() - pts[i].x(), projections[i].y() - pts[i].y(), projections[i].z() - pts[i].z() };
                double depth = v.norm();
                assert(std::abs(distances[i] - depth) < EPSILON);
                double radius = unscaled<double>(support_element_radius(config, element)) * scale;
                if (depth < radius) {
                    // Collision detected to be removed.
                    ++ num_moved;
                    double dxy = sqrt(sqr(radius) - sqr(v.z()));
                    double nudge_dist_max = dxy - std::hypot(v.x(), v.y())
                        //FIXME 1mm gap
                        + collision_extra_gap;
                    // Shift by maximum 2mm.
                    double nudge_dist = std::min(std::max(0., nudge_dist_max), max_nudge_collision_avoidance);
                    Vec2d nudge_v = to_2d(v).normalized() * (- nudge_dist);
                    pts[i].x() += nudge_v.x();
                    pts[i].y() += nudge_v.y();
                }
            }
            // Laplacian smoothing
            if (! locked && ! element.parents.empty()) {
                Vec2d avg{ 0, 0 };
                const SupportElements &above = move_bounds[element.state.layer_idx + 1];
                const size_t           offset_above = linear_data_layers[element.state.layer_idx + 1];
                double weight = 0.;
                for (auto iparent : element.parents) {
                    double w = support_element_radius(config, above[iparent]);
                    avg.x() += w * prev[offset_above + iparent].x();
                    avg.y() += w * prev[offset_above + iparent].y();
                    weight += w;
                }
                size_t cnt = element.parents.size();
                if (below != -1) {
                    const size_t offset_below = linear_data_layers[element.state.layer_idx - 1];
                    const double w = weight; //  support_element_radius(config, move_bounds[element.state.layer_idx - 1][below]);
                    avg.x() += w * prev[offset_below + below].x();
                    avg.y() += w * prev[offset_below + below].y();
                    ++ cnt;
                    weight += w;
                }
                //avg /= double(cnt);
                avg /= weight;
                static constexpr const double smoothing_factor = 0.5;
                Vec2d old_pos{ pts[i].x(), pts[i].y() };
                Vec2d new_pos = (1. - smoothing_factor) * old_pos + smoothing_factor * avg;
                Vec2d shift = new_pos - old_pos;
                double nudge_dist_max = shift.norm();
                // Shift by maximum 1mm, less than the collision avoidance factor.
                double nudge_dist = std::min(std::max(0., nudge_dist_max), max_nudge_smoothing);
                Vec2d nudge_v = shift.normalized() * nudge_dist;
                pts[i].x() += nudge_v.x();
                pts[i].y() += nudge_v.y();
            }
        }
//            printf("iteration: %d, moved: %d\n", int(iter), int(num_moved));
        if (num_moved == 0)
            break;
    }

    for (size_t i = 0; i < projections.size(); ++ i) {
        elements_with_link_down[i].first->state.result_on_layer.x() = scaled<coord_t>(pts[i].x()) / scale;
        elements_with_link_down[i].first->state.result_on_layer.y() = scaled<coord_t>(pts[i].y()) / scale;
    }
}
#endif // TREE_SUPPORT_ORGANIC_NUDGE_NEW

/*!
 * \brief Create the areas that need support.
 *
 * These areas are stored inside the given SliceDataStorage object.
 * \param storage The data storage where the mesh data is gotten from and
 * where the resulting support areas are stored.
 */
static void reconnect_organic_support_paths(PrintObject &object, TreeModelVolumes &volumes,
    const TreeSupportSettings &config, const OrganicSupportLayerPlan &plan,
    const SupportParameters &support_params, const SupportGeneratorLayersPtr &structural_layers,
    std::function<void()> throw_on_cancel);

static void generate_support_areas(Print &print, TreeSupport* tree_support, const BuildVolume &build_volume, const std::vector<size_t> &print_object_ids, std::function<void()> throw_on_cancel)
{
    trace_organic_timing("support_areas.begin");
    // Settings with the indexes of meshes that use these settings.
    std::vector<std::pair<TreeSupportSettings, std::vector<size_t>>> grouped_meshes = group_meshes(print, print_object_ids);
    trace_organic_timing("support_areas.grouped");
    if (grouped_meshes.empty())
        return;

    size_t counter = 0;

    // Process every mesh group. These groups can not be processed parallel, as the processing in each group is parallelized, and nested parallelization is disables and slow.
    for (std::pair<TreeSupportSettings, std::vector<size_t>> &processing : grouped_meshes)
    {
        trace_organic_timing("group.begin");
        // process each combination of meshes
        // this struct is used to easy retrieve setting. No other function except those in TreeModelVolumes and generate_initial_areas() have knowledge of the existence of multiple meshes being processed.
        //FIXME this is a copy
        // Contains config settings to avoid loading them in every function. This was done to improve readability of the code.
        const TreeSupportSettings &config = processing.first;
        BOOST_LOG_TRIVIAL(info) << "Processing support tree mesh group " << counter + 1 << " of " << grouped_meshes.size() << " containing " << grouped_meshes[counter].second.size() << " meshes.";
        auto t_start = std::chrono::high_resolution_clock::now();
#if 0
        std::vector<Polygons> exclude(num_support_layers);
        // get all already existing support areas and exclude them
        tbb::parallel_for(tbb::blocked_range<size_t>(0, num_support_layers),
            [&](const tbb::blocked_range<size_t> &range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++ layer_idx) {
                Polygons exlude_at_layer;
                append(exlude_at_layer, storage.support.supportLayers[layer_idx].support_bottom);
                append(exlude_at_layer, storage.support.supportLayers[layer_idx].support_roof);
                for (auto part : storage.support.supportLayers[layer_idx].support_infill_parts)
                    append(exlude_at_layer, part.outline);
                exclude[layer_idx] = union_(exlude_at_layer);
            }
        });
#endif
#ifdef SLIC3R_TREESUPPORTS_PROGRESS
        m_progress_multiplier = 1.0 / double(grouped_meshes.size());
        m_progress_offset = counter == 0 ? 0 : TREE_PROGRESS_TOTAL * (double(counter) * m_progress_multiplier);
#endif // SLIC3R_TREESUPPORT_PROGRESS
        PrintObject &print_object = *print.get_object(processing.second.front());
        // Generator for model collision, avoidance and internal guide volumes.
        TreeModelVolumes volumes{ print_object, build_volume, config.maximum_move_distance, config.maximum_move_distance_slow, processing.second.front(),
#ifdef SLIC3R_TREESUPPORTS_PROGRESS
            m_progress_multiplier, m_progress_offset,
#endif // SLIC3R_TREESUPPORTS_PROGRESS
            /* additional_excluded_areas */{} };
        trace_organic_timing("group.volumes.ready");

        //FIXME generating overhangs just for the first mesh of the group.
        assert(processing.second.size() == 1);

#if 1
        // use smart overhang detection
        std::vector<Polygons>        overhangs;
        std::vector<Polygons>        sharp_tail_overhangs;
        trace_organic_timing("overhangs.begin");
        tree_support->detect_overhangs();
        trace_organic_timing("overhangs.end");
        const int       num_raft_layers = int(config.raft_layers.size());
        const int       num_layers = int(print_object.layer_count()) + num_raft_layers;
        overhangs.resize(num_layers);
        sharp_tail_overhangs.resize(num_layers);
        for (size_t i = 0; i < print_object.layer_count(); i++) {
            for (ExPolygon& expoly : print_object.get_layer(i)->loverhangs) {
                Polygons polys = to_polygons(expoly);
                const bool is_sharp_tail = tree_support->overhang_types[&expoly] == TreeSupport::SharpTail;
                // Keep the unexpanded footprint for the actual contact
                // exception.  The legacy +0.2 mm expansion is useful for
                // locating a tree branch, but must not enlarge printed
                // interface material into the model.
                if (is_sharp_tail)
                    append(sharp_tail_overhangs[i + num_raft_layers], polys);
                if (is_sharp_tail)
                    polys = offset(polys, scale_(0.2));
                append(overhangs[i + num_raft_layers], polys);
            }
        }
        // add vertical enforcer points
        std::vector<float> zs = zs_from_layers(print_object.layers());
        Polygon            base_circle = make_circle(scale_(0.5), SUPPORT_TREE_CIRCLE_RESOLUTION);
        for (auto &pt_and_normal :tree_support->m_vertical_enforcer_points) {
            auto pt     = pt_and_normal.first;
            auto normal = pt_and_normal.second; // normal seems useless
            auto iter   = std::lower_bound(zs.begin(), zs.end(), pt.z());
            if (iter != zs.end()) {
                size_t layer_nr = iter - zs.begin();
                if (layer_nr > 0 && layer_nr < print_object.layer_count()) {
                    Polygon circle = base_circle;
                    circle.translate(to_2d(pt).cast<coord_t>());
                    overhangs[layer_nr + num_raft_layers].emplace_back(std::move(circle));
                }
            }
        }
#else
        std::vector<Polygons>        overhangs = generate_overhangs(config, *print.get_object(processing.second.front()), throw_on_cancel);
#endif
        // ### Precalculate avoidances, collision etc.
        trace_organic_timing("precalculate.begin");
        size_t num_support_layers = precalculate(print, overhangs, processing.first, processing.second, volumes, throw_on_cancel);
        trace_organic_timing("precalculate.end");
        bool   has_support = num_support_layers > 0;
        bool   has_raft    = config.raft_layers.size() > 0;
        num_support_layers = std::max(num_support_layers, config.raft_layers.size());

        if (num_support_layers == 0)
            continue;

        SupportParameters            support_params(print_object);
        support_params.with_sheath = true;
// Don't override the support density of tree supports, as the support density is used for raft.
// The trees will have the density zeroed in tree_supports_generate_paths()
//        support_params.support_density = 0;


        SupportGeneratorLayerStorage layer_storage;
        SupportGeneratorLayersPtr    top_contacts;
        SupportGeneratorLayersPtr    bottom_contacts;
        SupportGeneratorLayersPtr    interface_layers;
        SupportGeneratorLayersPtr    base_interface_layers;
        SupportGeneratorLayersPtr    intermediate_layers(num_support_layers, nullptr);
        OrganicSupportLayerPlan      output_plan;
        bool                         organic_source_repaired = false;
        if (support_params.has_top_contacts || has_raft)
            top_contacts.assign(num_support_layers, nullptr);
        if (support_params.has_bottom_contacts)
            bottom_contacts.assign(num_support_layers, nullptr);
        if (support_params.has_interfaces() || has_raft)
            interface_layers.assign(num_support_layers, nullptr);
        if (support_params.has_base_interfaces() || has_raft)
            base_interface_layers.assign(num_support_layers, nullptr);

        auto remove_undefined_layers = [&bottom_contacts, &top_contacts, &interface_layers, &base_interface_layers, &intermediate_layers]() {
            auto doit = [](SupportGeneratorLayersPtr& layers) {
                layers.erase(std::remove_if(layers.begin(), layers.end(), [](const SupportGeneratorLayer* ptr) { return ptr == nullptr; }), layers.end());
            };
            doit(bottom_contacts);
            doit(top_contacts);
            doit(interface_layers);
            doit(base_interface_layers);
            doit(intermediate_layers);
        };

        InterfacePlacer              interface_placer{
            print_object.slicing_parameters(), support_params, config,
            // Outputs
            layer_storage, top_contacts, interface_layers, base_interface_layers };

        if (has_support) {
            auto t_precalc = std::chrono::high_resolution_clock::now();

            // value is the area where support may be placed. As this is calculated in CreateLayerPathing it is saved and reused in draw_areas
            std::vector<SupportElements> move_bounds(num_support_layers);

            // ### Place tips of the support tree
            trace_organic_timing("initial_areas.begin");
            for (size_t mesh_idx : processing.second)
                generate_initial_areas(*print.get_object(mesh_idx), volumes, config, overhangs, sharp_tail_overhangs,
                    move_bounds, interface_placer, throw_on_cancel);
            trace_organic_timing("initial_areas.end");
            auto t_gen = std::chrono::high_resolution_clock::now();


            // ### Propagate the influence areas downwards. This is an inherently serial operation.
            print.set_status(60, _L("Generating support"));
            trace_organic_timing("layer_pathing.begin");
            create_layer_pathing(volumes, config, move_bounds, throw_on_cancel);
            trace_organic_timing("layer_pathing.end");
            auto t_path = std::chrono::high_resolution_clock::now();

            // ### Set a point in each influence area
            trace_organic_timing("nodes.begin");
            create_nodes_from_area(volumes, config, move_bounds, throw_on_cancel);
            trace_organic_timing("nodes.end");
            auto t_place = std::chrono::high_resolution_clock::now();

            // ### draw these points as circles
            // this new function give correct result when raft is also enabled
            trace_organic_timing("draw.begin");
            output_plan = organic_draw_branches(
                *print.get_object(processing.second.front()), volumes, config, move_bounds,
                bottom_contacts, top_contacts, interface_placer, intermediate_layers, layer_storage,
                throw_on_cancel, &organic_source_repaired);
            trace_organic_timing("draw.end");

            //tree_support->move_bounds_to_contact_nodes(move_bounds, print_object, config);

            trace_organic_stage("drawn", print_object, {{"bottom", &bottom_contacts}, {"top", &top_contacts},
                {"top_interface", &interface_layers}, {"base", &intermediate_layers}});

            remove_undefined_layers();

            if (output_plan.independent())
                normalize_organic_bottom_contacts(print_object, volumes, config, support_params, output_plan,
                    bottom_contacts, intermediate_layers, layer_storage);

            trace_organic_stage("normalized", print_object, {{"bottom", &bottom_contacts}, {"top", &top_contacts},
                {"top_interface", &interface_layers}, {"base", &intermediate_layers}});

            trace_organic_timing("interfaces.begin");
            std::tie(interface_layers, base_interface_layers) = generate_interface_layers(print_object.config(), support_params,
                bottom_contacts, top_contacts, interface_layers, base_interface_layers, intermediate_layers, layer_storage);
            trace_organic_timing("interfaces.end");
            trace_organic_stage("interfaces", print_object, {{"bottom", &bottom_contacts}, {"top", &top_contacts},
                {"interface", &interface_layers}, {"base", &intermediate_layers}});
            auto t_draw = std::chrono::high_resolution_clock::now();
            auto dur_pre_gen = 0.001 * std::chrono::duration_cast<std::chrono::microseconds>(t_precalc - t_start).count();
            auto dur_gen = 0.001 * std::chrono::duration_cast<std::chrono::microseconds>(t_gen - t_precalc).count();
            auto dur_path = 0.001 * std::chrono::duration_cast<std::chrono::microseconds>(t_path - t_gen).count();
            auto dur_place = 0.001 * std::chrono::duration_cast<std::chrono::microseconds>(t_place - t_path).count();
            auto dur_draw = 0.001 * std::chrono::duration_cast<std::chrono::microseconds>(t_draw - t_place).count();
            auto dur_total = 0.001 * std::chrono::duration_cast<std::chrono::microseconds>(t_draw - t_start).count();
            BOOST_LOG_TRIVIAL(info) <<
                "Total time used creating Tree support for the currently grouped meshes: " << dur_total << " ms. "
                "Different subtasks:\nCalculating Avoidance: " << dur_pre_gen << " ms "
                "Creating inital influence areas: " << dur_gen << " ms "
                "Influence area creation: " << dur_path << "ms "
                "Placement of Points in InfluenceAreas: " << dur_place << "ms "
                "Drawing result as support " << dur_draw << " ms";
    //        if (config.branch_radius==2121)
    //            BOOST_LOG_TRIVIAL(error) << "Why ask questions when you already know the answer twice.\n (This is not a real bug, please dont report it.)";
            
            move_bounds.clear();
        } else if (generate_raft_contact(print_object, config, interface_placer) >= 0) {
            remove_undefined_layers();
        } else
            // No raft.
            continue;

        // Produce the support G-code.
        trace_organic_timing("support_layers.begin");
        SupportGeneratorLayersPtr raft_layers = generate_raft_base(print_object, support_params, print_object.slicing_parameters(), top_contacts, interface_layers, base_interface_layers, intermediate_layers, layer_storage);
        SupportGeneratorLayersPtr layers_sorted = generate_support_layers(print_object, raft_layers, bottom_contacts, top_contacts, intermediate_layers, interface_layers, base_interface_layers);
        trace_organic_timing("support_layers.end");

        // BBS: This is a hack to avoid the support being generated outside the bed area. See #4769.
        tbb::parallel_for_each(layers_sorted.begin(), layers_sorted.end(), [&](SupportGeneratorLayer *layer) {
            if (layer) layer->polygons = intersection(layer->polygons, volumes.m_bed_area);
        });

        // Belt floor: clip ALL organic support layers (including intermediate/base
        // fill) against the belt surface.  The branch slices were already clipped
        // in organic_draw_branches(), but intermediate layers generated between
        // branches and the build plate need clipping too.
        // Compute the belt floor polygon directly from each layer's print_z
        // rather than mapping to a layer index (avoids index mismatch issues).
        {
            const auto &sp   = print_object.slicing_parameters();
            const auto &pcfg = print_object.print()->config();
            BeltFloorContext ctx;
            ctx.init_local(sp, pcfg, print_object.belt_global_z_offset());
            if (ctx.is_active()) {
                tbb::parallel_for_each(layers_sorted.begin(), layers_sorted.end(), [&](SupportGeneratorLayer *layer) {
                    if (!layer || layer->polygons.empty())
                        return;
                    layer->polygons = diff(layer->polygons, ctx.surface_polygon(layer->print_z));
                    if (layer->layer_type == SupporLayerType::BottomContact && layer->contact_polygons)
                        *layer->contact_polygons = diff(*layer->contact_polygons, ctx.surface_polygon(layer->print_z));
                });
            }
        }

        print.set_status(69, _L("Generating support"));
        trace_organic_stage("toolpath_input", print_object, {{"bottom", &bottom_contacts}, {"top", &top_contacts},
            {"interface", &interface_layers}, {"base", &intermediate_layers}});
        const auto t_toolpaths = std::chrono::steady_clock::now();
        trace_organic_timing("toolpaths.begin");
        const size_t physical_height_adjustments = generate_support_toolpaths(
            print_object.support_layers(), print_object.config(), support_params, print_object.slicing_parameters(),
            raft_layers, bottom_contacts, top_contacts, intermediate_layers, interface_layers, base_interface_layers);
        trace_organic_timing("toolpaths.end");
        const auto t_reconnect = std::chrono::steady_clock::now();
        const bool strict_printed_repair = std::getenv("INLONG_SUPPORT_FULL_REPAIR") != nullptr;
        if (strict_printed_repair || physical_height_adjustments > 0) {
            trace_organic_timing("reconnect.begin");
            reconnect_organic_support_paths(print_object, volumes, config, output_plan, support_params, layers_sorted, throw_on_cancel);
            trace_organic_timing("reconnect.end");
        } else
            BOOST_LOG_TRIVIAL(info) << "Organic support toolpaths: printed-path repair skipped; native topology path="
                << (organic_source_repaired ? "repaired" : "unchanged")
                << ", physical-height adjustments=" << physical_height_adjustments
                << ", strict=" << strict_printed_repair;
        BOOST_LOG_TRIVIAL(info) << "Organic support toolpaths: generation " <<
            std::chrono::duration<double>(t_reconnect - t_toolpaths).count() << " seconds, reconnection " <<
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t_reconnect).count() << " seconds";

        // Optional local profiling payload, inactive in ordinary slicing.
        if (const char *profile_path = std::getenv("INLONG_SUPPORT_SHAPE_PROFILE_PATH")) {
            std::ofstream profile(profile_path, std::ios::binary);
            auto write = [&profile](const auto &value) {
                profile.write(reinterpret_cast<const char *>(&value), sizeof(value));
            };
            auto write_polygon = [&write](const Polygon &polygon) {
                write(uint32_t(polygon.size()));
                for (const Point &point : polygon.points) {
                    write(int64_t(point.x()));
                    write(int64_t(point.y()));
                }
            };
            write(uint32_t(print_object.support_layers().size()));
            for (const SupportLayer *layer : print_object.support_layers()) {
                write(layer->print_z);
                write(layer->height);
                write(uint32_t(layer->support_islands.size()));
                for (const ExPolygon &island : layer->support_islands) {
                    write_polygon(island.contour);
                    write(uint32_t(island.holes.size()));
                    for (const Polygon &hole : island.holes)
                        write_polygon(hole);
                }
            }
        }
        auto t_end = std::chrono::high_resolution_clock::now();
        BOOST_LOG_TRIVIAL(info) << "Total time of organic tree support: " << 0.001 * std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count() << " ms";
 #if 0
//#ifdef SLIC3R_DEBUG
        {
            static int iRun = 0;
            ++ iRun;
            size_t layer_id = 0;
            for (int i = 0; i < int(layers_sorted.size());) {
                // Find the last layer with roughly the same print_z, find the minimum layer height of all.
                // Due to the floating point inaccuracies, the print_z may not be the same even if in theory they should.
                int j = i + 1;
                coordf_t zmax = layers_sorted[i]->print_z + EPSILON;
                bool empty = layers_sorted[i]->polygons.empty();
                for (; j < layers_sorted.size() && layers_sorted[j]->print_z <= zmax; ++j)
                    if (!layers_sorted[j]->polygons.empty())
                        empty = false;
                if (!empty) {
                    export_print_z_polygons_to_svg(
                        debug_out_path("support-%d-%lf.svg", iRun, layers_sorted[i]->print_z).c_str(),
                        layers_sorted.data() + i, j - i);
                    export_print_z_polygons_and_extrusions_to_svg(
                        debug_out_path("support-w-fills-%d-%lf.svg", iRun, layers_sorted[i]->print_z).c_str(),
                        layers_sorted.data() + i, j - i,
                        *print_object.support_layers()[layer_id]);
                    ++layer_id;
                }
                i = j;
            }
        }
#endif /* SLIC3R_DEBUG */

        trace_organic_timing("group.end");
        ++ counter;
    }

//   storage.support.generated = true;
    trace_organic_timing("support_areas.end");
}

static void recover_pending_branch_roofs(
    InterfacePlacer        &interface_placer,
    const std::vector<const SupportElement*> &branch_path,
    const LayerIndex        layer_begin,
    const OrganicSupportLayerPlan &layer_plan,
    std::vector<Polygons>  &slices)
{
    if (! interface_placer.support_parameters.has_top_contacts)
        return;

    for (auto it = branch_path.rbegin(); it != branch_path.rend(); ++ it) {
        const SupportElement &el = **it;
        if (! el.state.has_pending_roof_recovery())
            break;

        const coordf_t source_z = layer_z(interface_placer.slicing_parameters, interface_placer.config, el.state.layer_idx);
        const LayerIndex output_layer_idx = layer_plan.at_or_below(source_z);
        const LayerIndex slice_idx = output_layer_idx - layer_begin;
        if (slice_idx < 0 || slice_idx >= LayerIndex(slices.size()))
            continue;
        if (slices[size_t(slice_idx)].empty())
            continue;
        if (el.state.roof_recovery_dtt > interface_placer.support_parameters.num_top_interface_layers)
            continue;

        interface_placer.add_roof(std::move(slices[size_t(slice_idx)]), size_t(output_layer_idx), el.state.roof_recovery_dtt);
    }
}

// Collision avoidance proves that a branch is clear of the model, not that its
// first printable layer rests on anything. In particular a curved surface's XY
// clearance can consume a descending root before it reaches its Z contact gap.
// Join each disconnected component downwards, within the branch angle and the
// configured clearances, before bottom/interface layers are propagated.
static Polygons organic_support_slab_collision(const PrintObject &object, TreeModelVolumes &volumes,
    const TreeSupportSettings &config, const SupportParameters &support_params, double bottom_z, double top_z)
{
    Polygons polygons;
    const auto &layers = object.layers();
    const auto &slicing = object.slicing_parameters();
    const double z_offset = object.belt_global_z_offset();
    auto first = std::lower_bound(layers.begin(), layers.end(), bottom_z - slicing.gap_object_support + z_offset + EPSILON,
        [](const Layer *layer, double z) { return layer->print_z < z; });
    for (auto it = first; it != layers.end(); ++it) {
        const Layer &layer = **it;
        if (layer.bottom_z() - z_offset >= top_z + slicing.gap_support_object - EPSILON)
            break;
        const bool overlaps = layer.print_z - z_offset > bottom_z + EPSILON &&
            layer.bottom_z() - z_offset < top_z - EPSILON;
        if (overlaps) {
            const coord_t xy_gap = bottom_z <= EPSILON ? scale_(support_params.gap_xy_first_layer) : config.xy_distance;
            append(polygons, offset(layer.lslices, xy_gap, jtRound, scale_(0.01)));
            const size_t source_idx = config.raft_layers.size() + size_t(std::distance(layers.begin(), it));
            const auto &blockers = volumes.support_blockers();
            if (source_idx < blockers.size())
                append(polygons, blockers[source_idx]);
        } else
            append(polygons, to_polygons(layer.lslices));
    }
    return polygons.empty() ? Polygons{} : union_(polygons);
}

static bool anchor_organic_support_components(
    const PrintObject &object, TreeModelVolumes &volumes, const TreeSupportSettings &config,
    const OrganicSupportLayerPlan &plan, const SupportParameters &support_params,
    SupportGeneratorLayersPtr &bottom_contacts, const SupportGeneratorLayersPtr &top_contacts,
    const SupportGeneratorLayersPtr &top_interfaces, const SupportGeneratorLayersPtr &top_base_interfaces,
    SupportGeneratorLayersPtr &intermediate_layers, SupportGeneratorLayerStorage &storage,
    std::function<void()> throw_on_cancel)
{
    const auto anchor_profile_started = std::chrono::steady_clock::now();
    if (plan.size() < 2)
        return false;
    const double min_area = sqr(double(config.support_line_width)) * 0.05;
    std::vector<size_t> suspect_layers;

    // Most independent organic plans are already connected by the rasterized
    // base layers.  Verify that cheaply on the consecutive layer graph before
    // constructing the expensive per-component/lower-layer caches.  This is a
    // conservative check: the full landing repair remains enabled whenever a
    // later base component has no exact predecessor (or a layer gap occurs).
    {
        ExPolygons previous_components;
        bool disconnected = false;
        for (size_t idx = 0; idx < plan.size(); ++idx) {
            const Polygons *current_polygons =
                idx < intermediate_layers.size() && intermediate_layers[idx] != nullptr ?
                    &intermediate_layers[idx]->polygons : nullptr;
            if (current_polygons == nullptr || current_polygons->empty())
                continue;
            ExPolygons current_components = union_ex(*current_polygons);
            if (previous_components.empty()) {
                previous_components = std::move(current_components);
                continue;
            }
            if (idx < bottom_contacts.size() && bottom_contacts[idx] != nullptr &&
                !bottom_contacts[idx]->polygons.empty()) {
                previous_components = std::move(current_components);
                continue;
            }
            // A tree branch is allowed to move laterally between adjacent
            // layers.  Requiring a raw polygon intersection here marks
            // ordinary sloped trunks as disconnected and sends the whole
            // model through the expensive anchoring walk.  Use the same
            // per-layer movement allowance as the real repair path before
            // deciding that a component is genuinely floating.
            const double dz = plan[idx].print_z - plan[idx - 1].print_z;
            const coord_t movement = coord_t(std::max(0., dz * double(config.maximum_move_distance) /
                unscaled<double>(config.layer_height)));
            const coord_t reach = movement + std::max<coord_t>(scale_(0.01), config.support_line_width / 2);
            std::vector<BoundingBox> reachable_previous;
            reachable_previous.reserve(previous_components.size());
            for (const ExPolygon &previous : previous_components) {
                BoundingBox bounds = get_extents(previous);
                if (!bounds.defined)
                    continue;
                bounds.min.x() -= reach;
                bounds.min.y() -= reach;
                bounds.max.x() += reach;
                bounds.max.y() += reach;
                reachable_previous.push_back(bounds);
            }
            bool layer_disconnected = false;
            for (const ExPolygon &component : current_components) {
                const BoundingBox component_bounds = get_extents(component);
                bool predecessor = false;
                for (const BoundingBox &previous_bounds : reachable_previous) {
                    // This is only a conservative prefilter.  The exact
                    // collision/landing tests remain in the repair walk for
                    // layers which fall outside the reachable envelope.
                    if (component_bounds.defined && component_bounds.overlap(previous_bounds)) {
                        predecessor = true;
                        break;
                    }
                }
                if (!predecessor) {
                    disconnected = true;
                    layer_disconnected = true;
                    break;
                }
            }
            if (layer_disconnected)
                suspect_layers.push_back(idx);
            previous_components = std::move(current_components);
        }
        if (!disconnected) {
            trace_organic_timing("anchor.fast_path.connected");
            return false;
        }
        trace_organic_timing("anchor.fast_path.repair_needed");
    }
    const SupportGeneratorLayersPtr *sources[] =
        {&bottom_contacts, &top_contacts, &top_interfaces, &top_base_interfaces, &intermediate_layers};
    // Retain only the current cross-sections, not another copy of every support
    // layer.  Cache the derived footprint once per layer: the anchoring walk
    // asks for the same lower layers from many components, and recomputing the
    // Clipper union for each request turns large organic trees into a quadratic
    // workload without changing any geometry.
    std::vector<Polygons> footprint_cache(plan.size());
    std::vector<bool> footprint_ready(plan.size(), false);
    std::vector<BoundingBox> footprint_bounds(plan.size());
    std::vector<bool> footprint_bounds_ready(plan.size(), false);
    size_t footprint_builds = 0;
    size_t collision_builds = 0;
    size_t bbox_rejections = 0;
    size_t exact_overlap_tests = 0;
    auto invalidate_footprint = [&](size_t idx) {
        if (idx >= plan.size())
            return;
        footprint_ready[idx] = false;
        footprint_cache[idx].clear();
        footprint_bounds_ready[idx] = false;
        footprint_bounds[idx] = BoundingBox{};
    };
    auto footprint = [&](size_t idx) -> const Polygons & {
        if (!footprint_ready[idx]) {
            ++footprint_builds;
            Polygons polygons;
            size_t contributors = 0;
            const Polygons *single_source = nullptr;
            for (const SupportGeneratorLayersPtr *layers : sources) {
                if (idx < layers->size() && (*layers)[idx] != nullptr && !(*layers)[idx]->polygons.empty()) {
                    ++contributors;
                    single_source = &(*layers)[idx]->polygons;
                    append(polygons, (*layers)[idx]->polygons);
                }
            }
            // Every intermediate/base layer is already normalized by the final
            // assembly.  When it is the only contributor, avoid rebuilding the
            // same Clipper union for every anchoring query.  Multiple source
            // roles still take the exact historical union path.
            if (contributors == 0)
                footprint_cache[idx] = Polygons{};
            else if (contributors == 1 && single_source != nullptr)
                footprint_cache[idx] = intersection(*single_source, volumes.m_bed_area);
            else
                footprint_cache[idx] = intersection(union_(polygons), volumes.m_bed_area);
            footprint_ready[idx] = true;
        }
        return footprint_cache[idx];
    };
    auto footprint_box = [&](size_t idx) -> const BoundingBox & {
        if (!footprint_bounds_ready[idx]) {
            const Polygons &polygons = footprint(idx);
            footprint_bounds[idx] = polygons.empty() ? BoundingBox{} : get_extents(polygons);
            footprint_bounds_ready[idx] = true;
        }
        return footprint_bounds[idx];
    };
    const double object_z_offset = object.belt_global_z_offset();
    const auto &model_layers = object.layers();
    std::vector<BoundingBox> model_layer_bounds;
    model_layer_bounds.reserve(model_layers.size());
    for (const Layer *layer : model_layers)
        model_layer_bounds.push_back(get_extents(layer->lslices));
    std::vector<Polygons> collision_cache(plan.size());
    std::vector<bool> collision_ready(plan.size(), false);
    auto collision = [&](size_t idx) -> const Polygons & {
        if (!collision_ready[idx]) {
            ++collision_builds;
            Polygons polygons;
            // The topology cache rounds Z distances to source layers. A union
            // of those masks can reserve an extra model layer at an independent
            // output height, leaving no route under an otherwise valid roof.
            // Test physical extrusion slabs instead: XY clearance belongs to
            // overlapping material, while separated slabs retain their Z gap.
            const auto &entry = plan[idx];
            const auto &layers = object.layers();
            const auto &slicing = object.slicing_parameters();
            auto first = std::lower_bound(layers.begin(), layers.end(),
                entry.bottom_z - slicing.gap_object_support + object_z_offset + EPSILON,
                [](const Layer *layer, double z) { return layer->print_z < z; });
            for (auto it = first; it != layers.end(); ++it) {
                const Layer &layer = **it;
                if (layer.bottom_z() - object_z_offset >= entry.print_z + slicing.gap_support_object - EPSILON)
                    break;
                const bool overlaps = layer.print_z - object_z_offset > entry.bottom_z + EPSILON &&
                    layer.bottom_z() - object_z_offset < entry.print_z - EPSILON;
                if (overlaps) {
                    const coord_t xy_gap = entry.bottom_z <= EPSILON ?
                        scale_(support_params.gap_xy_first_layer) : config.xy_distance;
                    append(polygons, offset(layer.lslices, xy_gap, jtRound, scale_(0.01)));
                    const size_t source_idx = config.raft_layers.size() + size_t(std::distance(layers.begin(), it));
                    const auto &blockers = volumes.support_blockers();
                    if (source_idx < blockers.size())
                        append(polygons, blockers[source_idx]);
                } else {
                    append(polygons, to_polygons(layer.lslices));
                }
            }
            collision_cache[idx] = polygons.empty() ? Polygons{} : union_(polygons);
            collision_ready[idx] = true;
        }
        return collision_cache[idx];
    };
    auto exact_overlap = [&](const Polygons &lhs, const BoundingBox &lhs_bounds,
                             const Polygons &rhs, const BoundingBox &rhs_bounds) {
        if (lhs_bounds.defined && rhs_bounds.defined && !lhs_bounds.overlap(rhs_bounds)) {
            ++bbox_rejections;
            return false;
        }
        ++exact_overlap_tests;
        return Slic3r::area(intersection(lhs, rhs)) > min_area;
    };
    auto model_landing = [&](double bottom_z, const Polygons &region, const BoundingBox &region_bounds) {
        if (!config.support_rests_on_model)
            return false;
        const double below = bottom_z - object.slicing_parameters().gap_object_support + object_z_offset;
        auto it = std::upper_bound(model_layers.begin(), model_layers.end(), below + EPSILON,
            [](double z, const Layer *layer) { return z < layer->print_z; });
        if (it == model_layers.begin())
            return false;
        const size_t index = size_t(std::distance(model_layers.begin(), std::prev(it)));
        if (index >= model_layer_bounds.size() || !region_bounds.defined ||
            !model_layer_bounds[index].defined || !region_bounds.overlap(model_layer_bounds[index]))
            return false;
        const Polygons model = to_polygons((*std::prev(it))->lslices);
        return exact_overlap(region, region_bounds, model, model_layer_bounds[index]);
    };
    BeltFloorContext belt;
    belt.init_local(object.slicing_parameters(), object.print()->config(), object.belt_global_z_offset());
    auto grounded = [&](size_t idx, double bottom_z, const Polygons &region, const BoundingBox &region_bounds) {
        return (!belt.is_active() && bottom_z <= object.slicing_parameters().raft_contact_top_z + EPSILON) || model_landing(bottom_z, region, region_bounds) ||
            (belt.is_active() && Slic3r::area(intersection(region, belt.surface_polygon(bottom_z))) > min_area);
    };
    auto add_base = [&](size_t idx, const Polygons &area) {
        if (intermediate_layers[idx] == nullptr) {
            intermediate_layers[idx] = &storage.allocate(SupporLayerType::Base);
            plan.initialize(*intermediate_layers[idx], idx);
        }
        intermediate_layers[idx]->polygons = union_(intermediate_layers[idx]->polygons, area);
        invalidate_footprint(idx);
    };
    auto footprint_components = [&](size_t idx) {
        const Polygons &polygons = footprint(idx);
        // The final assembly emits a single already-normalized contour for
        // most layers. Avoid rebuilding an ExPolygon union in that common
        // case; retain the exact union path whenever there are multiple
        // contours (where holes or touching islands still need Clipper).
        if (polygons.size() == 1)
            return ExPolygons{ExPolygon(polygons.front())};
        return union_ex(polygons);
    };
    size_t repaired = 0;
    size_t relocated = 0;
    for (size_t idx = 1; idx < plan.size(); ++idx) {
        if (!suspect_layers.empty() &&
            !std::binary_search(suspect_layers.begin(), suspect_layers.end(), idx))
            continue;
        if (idx == 1 || idx + 1 == plan.size() || idx % 50 == 0)
            trace_organic_timing_progress("anchor.layers", idx, plan.size());
        throw_on_cancel();
        const ExPolygons components = footprint_components(idx);
        for (const ExPolygon &component : components) {
            Polygons current = to_polygons(component);
            const BoundingBox component_bounds = get_extents(component);
            BoundingBox current_bounds = component_bounds;
            double bottom_z = plan[idx].bottom_z;
            for (const SupportGeneratorLayersPtr *layers : sources)
                if (idx < layers->size() && (*layers)[idx] != nullptr &&
                    exact_overlap(current, current_bounds, (*layers)[idx]->polygons,
                        get_extents((*layers)[idx]->polygons)))
                    bottom_z = std::min(bottom_z, (*layers)[idx]->bottom_print_z());
            bool has_predecessor = false;
            for (size_t below = idx; below-- > 0 && plan[below].print_z >= bottom_z - EPSILON;)
                if (exact_overlap(current, current_bounds, footprint(below), footprint_box(below))) {
                    has_predecessor = true;
                    break;
                }
            if (component.area() <= min_area || grounded(idx, bottom_z, current, current_bounds) || has_predecessor)
                continue;
            // A former "bottom contact" at this height is now an ordinary part
            // of the continuation; the real bottom interface belongs at its landing.
            if (idx < bottom_contacts.size() && bottom_contacts[idx] != nullptr) {
                Polygons former_contact = intersection(bottom_contacts[idx]->polygons, current);
                bottom_contacts[idx]->polygons = diff(bottom_contacts[idx]->polygons, current);
                if (!former_contact.empty())
                    add_base(idx, former_contact);
            }
            bool landed = false;
            bool joins_model = false;
            std::vector<std::pair<size_t, Polygons>> continuation;
            for (size_t lower = idx; lower-- > 0;) {
                const double dz = plan[lower + 1].print_z - plan[lower].print_z;
                const coord_t movement = coord_t(std::max(0., dz * double(config.maximum_move_distance) /
                    unscaled<double>(config.layer_height)));
                // A clear vertical landing does not need a wider trunk. The
                // former unconditional outset accumulated one movement radius
                // per layer and welded otherwise independent roots together.
                Polygons reachable = diff_clipped(current, collision(lower), ApplySafetyOffset::Yes);
                if (area(diff(current, reachable)) > min_area)
                    reachable = diff_clipped(offset(current, movement, jtRound, scale_(0.01)),
                        collision(lower), ApplySafetyOffset::Yes);
                reachable = intersection(reachable, volumes.m_bed_area);
                if (belt.is_active())
                    reachable = diff(reachable, belt.surface_polygon(plan[lower].print_z));
                // Keep only continuations which actually overlap the layer above.
                Polygons connected;
                for (const ExPolygon &part : union_ex(reachable)) {
                    Polygons polygons = to_polygons(part);
                    const BoundingBox part_bounds = get_extents(part);
                    if (exact_overlap(polygons, part_bounds, current, current_bounds))
                        append(connected, std::move(polygons));
                }
                if (connected.empty())
                    break;
                const BoundingBox connected_bounds = get_extents(connected);
                const bool joins_support = exact_overlap(connected, connected_bounds,
                    footprint(lower), footprint_box(lower));
                joins_model = model_landing(plan[lower].bottom_z, connected, connected_bounds);
                const bool reaches_ground = grounded(lower, plan[lower].bottom_z, connected, connected_bounds);
                continuation.emplace_back(lower, connected);
                if (joins_support || reaches_ground) {
                    landed = true;
                    break;
                }
                current = std::move(connected);
                current_bounds = connected_bounds;
            }
            if (!landed) {
                // The clipped end of a root may be too close to a side wall to
                // continue at the requested branch angle. Move that root up to
                // its next, wider section and plan its landing again. Required
                // top contact/interface material must never be discarded.
                bool required_roof = false;
                for (const SupportGeneratorLayersPtr *layers :
                     {&top_contacts, &top_interfaces, &top_base_interfaces}) {
                    if (idx >= layers->size() || (*layers)[idx] == nullptr)
                        continue;
                    const BoundingBox roof_bounds = get_extents((*layers)[idx]->polygons);
                    if (exact_overlap(to_polygons(component), component_bounds,
                        (*layers)[idx]->polygons, roof_bounds))
                        required_roof = true;
                }
                if (required_roof) {
                    const BoundingBox bounds = get_extents(component);
                    BOOST_LOG_TRIVIAL(warning) << "Organic support landing failed at Z=" << plan[idx].print_z <<
                        ", XY=" << unscale<double>(bounds.min.x()) << "," << unscale<double>(bounds.min.y()) <<
                        ".." << unscale<double>(bounds.max.x()) << "," << unscale<double>(bounds.max.y()) <<
                        ", planned continuation layers=" << continuation.size();
                    throw SlicingError("Organic support at Z=" + std::to_string(plan[idx].print_z) +
                        " cannot reach a valid landing with the configured clearances.");
                }
                if (intermediate_layers[idx] != nullptr)
                    intermediate_layers[idx]->polygons = diff(intermediate_layers[idx]->polygons, to_polygons(component));
                invalidate_footprint(idx);
                ++relocated;
                continue;
            }
            for (const auto &[lower, area] : continuation)
                add_base(lower, area);
            if (joins_model && support_params.num_bottom_interface_layers > 0) {
                const size_t lower = continuation.back().first;
                const Polygons &area = continuation.back().second;
                if (bottom_contacts.size() < plan.size())
                    bottom_contacts.resize(plan.size(), nullptr);
                if (bottom_contacts[lower] == nullptr) {
                    bottom_contacts[lower] = &storage.allocate(SupporLayerType::BottomContact);
                    plan.initialize(*bottom_contacts[lower], lower);
                }
                bottom_contacts[lower]->polygons = union_(bottom_contacts[lower]->polygons, area);
                intermediate_layers[lower]->polygons = diff(intermediate_layers[lower]->polygons, area);
                invalidate_footprint(lower);
            }
            ++repaired;
            throw_on_cancel();
        }
    }
    trace_organic_timing("anchor.components.end");
    BOOST_LOG_TRIVIAL(info) << "Organic support anchoring: repaired " << repaired
        << " disconnected components and relocated " << relocated << " clipped root ends"
        << ", layers=" << plan.size() << ", elapsed "
        << std::chrono::duration<double>(std::chrono::steady_clock::now() - anchor_profile_started).count()
        << " seconds, footprint builds=" << footprint_builds
        << ", collision builds=" << collision_builds
        << ", exact overlap tests=" << exact_overlap_tests
        << ", AABB rejections=" << bbox_rejections;
    return repaired > 0 || relocated > 0;
}

// Source-area intersections can join the empty interiors of two hollow trees.
// Fill missing output planes only when the actual beads have a collision-free
// vertical route to an already printed support. Existing paths are retained.
static void reconnect_organic_support_paths(PrintObject &object, TreeModelVolumes &volumes,
    const TreeSupportSettings &config, const OrganicSupportLayerPlan &plan,
    const SupportParameters &support_params, const SupportGeneratorLayersPtr &structural_layers,
    std::function<void()> throw_on_cancel)
{
    if (plan.size() < 2)
        return;
    const auto profile_started = std::chrono::steady_clock::now();
    const bool independent_organic = plan.independent();
    std::vector<SupportLayer *> output(plan.size(), nullptr);
    const bool trace_anchors = std::getenv("INLONG_SUPPORT_ANCHOR_TRACE") != nullptr;
    for (SupportLayer *layer : object.support_layers()) {
        const LayerIndex idx = plan.find_exact(layer->print_z);
        if (idx >= 0)
            output[size_t(idx)] = layer;
    }
    const double min_area = sqr(double(config.support_line_width)) * 0.002;
    const double object_z_offset = object.belt_global_z_offset();
    BeltFloorContext belt;
    belt.init_local(object.slicing_parameters(), object.print()->config(), object_z_offset);
    auto touches = [&](const Polygons &a, const Polygons &b) {
        // Use exact printed bead footprints; the former tolerance could
        // classify a real micro-gap as already anchored.
        return area(intersection(a, b)) > min_area;
    };
    struct BeadKey {
        Polyline line;
        float width;
        bool operator==(const BeadKey &other) const { return width == other.width && line.points == other.line.points; }
    };
    struct BeadHash {
        size_t operator()(const BeadKey &key) const {
            size_t hash = std::hash<float>{}(key.width);
            for (const Point &point : key.line.points)
                for (coord_t coordinate : {point.x(), point.y()})
                    hash ^= std::hash<coord_t>{}(coordinate) + size_t(0x9e3779b9) + (hash << 6) + (hash >> 2);
            return hash;
        }
    };
    // Share the existing live-memory allowance with other active objects.
    // Unknown/low-memory hosts use the original uncached calculation. Cached
    // values are addressed by full geometry, never by a mutable entity address.
    SupportBatchBudget bead_budget(size_t(64) << 20, support_memory_snapshot());
    const size_t road_cache_limit = bead_budget.bytes() * 3 / 4;
    const size_t layer_cache_limit = bead_budget.bytes() / 8;
    const size_t index_cache_limit = bead_budget.bytes() - road_cache_limit - layer_cache_limit;
    std::unordered_map<BeadKey, Polygons, BeadHash> bead_cache;
    size_t bead_cache_bytes = 0;
    auto polygon_bytes = [](const Polygons &polygons) {
        size_t bytes = polygons.capacity() * sizeof(Polygon) + 128;
        for (const Polygon &polygon : polygons) bytes += polygon.points.capacity() * sizeof(Point);
        return bytes;
    };
    auto printed_bead = [&](const ExtrusionPath &path) {
        BeadKey key{path.polyline.to_polyline(), path.width};
        if (road_cache_limit > 0) {
            const auto found = bead_cache.find(key);
            if (found != bead_cache.end()) return found->second;
        }
        // Square offset joins can falsely connect a thin isolated road to the
        // corner of its neighbour. Match the rounded deposited bead instead.
        Polygons bead = offset(key.line, float(scale_(path.width * 0.5)),
            jtRound, scale_(0.001), etOpenRound);
        const size_t bytes = polygon_bytes(bead) + key.line.points.capacity() * sizeof(Point) + sizeof(BeadKey);
        if (bytes <= road_cache_limit) {
            if (bead_cache_bytes + bytes > road_cache_limit) {
                bead_cache.clear();
                bead_cache_bytes = 0;
            }
            try {
                bead_cache.emplace(std::move(key), bead);
                bead_cache_bytes += bytes;
            } catch (const std::bad_alloc &) {
                bead_cache.clear();
                bead_cache_bytes = 0;
            }
        }
        return bead;
    };
    std::function<void(const ExtrusionEntity &, ExtrusionPaths &)> collect_paths;
    collect_paths = [&](const ExtrusionEntity &entity, ExtrusionPaths &paths) {
        if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
            // Both body and interface beads are valid bearing surfaces. The
            // repair candidates are filtered separately below, so an existing
            // interface anchors a trunk without being copied as ordinary fill.
            if (is_support(path->role()) && !path->is_force_no_extrusion() && path->mm3_per_mm > 0.)
                paths.push_back(*path);
        } else if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
            for (const ExtrusionEntity *child : collection->entities)
                collect_paths(*child, paths);
        } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
            for (const ExtrusionPath &path : loop->paths)
                collect_paths(path, paths);
        } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
            for (const ExtrusionPath &path : multi->paths)
                collect_paths(path, paths);
        }
    };
    std::optional<size_t> footprint_idx;
    Polygons footprint_polygons;
    std::unordered_map<size_t, Polygons> footprint_cache;
    size_t footprint_bytes = 0, footprint_hits = 0;
    const size_t footprint_limit = bead_budget.bytes() / 8;
    auto invalidate_footprints = [&] {
        footprint_idx.reset();
        footprint_cache.clear();
        footprint_bytes = 0;
    };
    auto footprint = [&](size_t idx) -> const Polygons & {
        if (!footprint_idx || *footprint_idx != idx) {
            const auto cached = footprint_cache.find(idx);
            if (cached != footprint_cache.end()) {
                footprint_polygons = cached->second;
                footprint_idx = idx;
                ++footprint_hits;
                return footprint_polygons;
            }
            Polygons polygons;
            if (output[idx] != nullptr) {
                ExtrusionPaths paths;
                collect_paths(output[idx]->support_fills, paths);
                for (const ExtrusionPath &path : paths)
                    append(polygons, printed_bead(path));
            }
            footprint_polygons = union_(polygons);
            footprint_idx = idx;
            const size_t bytes = polygon_bytes(footprint_polygons);
            if (bytes <= footprint_limit) {
                if (footprint_bytes + bytes > footprint_limit || footprint_cache.size() >= 8) {
                    footprint_cache.clear();
                    footprint_bytes = 0;
                }
                try {
                    footprint_cache.emplace(idx, footprint_polygons);
                    footprint_bytes += bytes;
                } catch (const std::bad_alloc &) {
                    footprint_cache.clear();
                    footprint_bytes = 0;
                }
            }
        }
        return footprint_polygons;
    };
    auto model_landing = [&](double bottom_z, const Polygons &area) {
        if (!independent_organic || !config.support_rests_on_model)
            return false;
        const double below = bottom_z - object.slicing_parameters().gap_object_support + object_z_offset;
        const auto &layers = object.layers();
        auto it = std::upper_bound(layers.begin(), layers.end(), below + EPSILON,
            [](double z, const Layer *layer) { return z < layer->print_z; });
        return it != layers.begin() &&
            Slic3r::area(intersection(area, to_polygons((*std::prev(it))->lslices))) > min_area;
    };
    // Corrected bottom contacts need not coincide with a branch-mesh plane.
    // They are real printed predecessors even though the topology plan has
    // no index for their Z. Do not discard a trunk standing on such a contact.
    std::vector<const SupportLayer *> contact_planes;
    for (const SupportLayer *layer : object.support_layers())
        if (plan.find_exact(layer->print_z) < 0)
            contact_planes.push_back(layer);
    std::sort(contact_planes.begin(), contact_planes.end(), [](const auto *a, const auto *b) {
        return a->print_z < b->print_z;
    });
    auto touches_contact_plane = [&](const Polygons &bead, double bottom, double top) {
        auto lower = std::lower_bound(contact_planes.begin(), contact_planes.end(), bottom - EPSILON,
            [](const auto *layer, double z) { return layer->print_z < z; });
        for (; lower != contact_planes.end() && (*lower)->print_z < top - EPSILON; ++lower) {
            Polygons printed;
            ExtrusionPaths paths;
            collect_paths((*lower)->support_fills, paths);
            for (const ExtrusionPath &path : paths)
                append(printed, printed_bead(path));
            if (touches(bead, union_(printed)))
                return true;
        }
        return false;
    };
    // Remove only an isolated contact remnant, retaining the collection/loop
    // structure and every unrelated extrusion on the same layer.
    std::function<bool(ExtrusionEntity &, const Polygons &, ExtrusionRole)> remove_fragment;
    remove_fragment = [&](ExtrusionEntity &entity, const Polygons &fragment, ExtrusionRole role) {
        auto belongs = [&](const ExtrusionPath &path) {
            return path.role() == role &&
                Slic3r::area(intersection(printed_bead(path), fragment)) > min_area;
        };
        if (auto *path = dynamic_cast<ExtrusionPath *>(&entity))
            return belongs(*path);
        if (auto *collection = dynamic_cast<ExtrusionEntityCollection *>(&entity)) {
            auto &entities = collection->entities;
            entities.erase(std::remove_if(entities.begin(), entities.end(), [&](ExtrusionEntity *child) {
                if (!remove_fragment(*child, fragment, role))
                    return false;
                delete child;
                return true;
            }), entities.end());
            return entities.empty();
        }
        ExtrusionPaths *paths = nullptr;
        if (auto *loop = dynamic_cast<ExtrusionLoop *>(&entity))
            paths = &loop->paths;
        else if (auto *multi = dynamic_cast<ExtrusionMultiPath *>(&entity))
            paths = &multi->paths;
        if (paths == nullptr)
            return false;
        paths->erase(std::remove_if(paths->begin(), paths->end(), belongs), paths->end());
        return paths->empty();
    };
    size_t repaired = 0;
    size_t removed = 0;
    size_t unresolved = 0;
    for (size_t idx = 1; idx < plan.size(); ++idx) {
        throw_on_cancel();
        if (output[idx] == nullptr)
            continue;
        ExtrusionPaths paths;
        collect_paths(output[idx]->support_fills, paths);
        std::vector<BoundingBox> path_bounds;
        path_bounds.reserve(paths.size());
        for (const ExtrusionPath &path : paths) {
            BoundingBox bounds = get_extents(path.polyline.to_polyline());
            if (bounds.defined)
                bounds.offset(coord_t(std::ceil(scale_(std::abs(path.width)))) + 4 * SCALED_EPSILON);
            path_bounds.push_back(bounds);
        }
        for (const ExPolygon &component : union_ex(footprint(idx))) {
            if (component.area() <= min_area)
                continue;
            const Polygons area = to_polygons(component);
            const BoundingBox component_bounds = get_extents(component);
            ExtrusionPaths selected;
            bool has_interface = false;
            bool anchored = false;
            for (size_t path_index = 0; path_index < paths.size(); ++path_index) {
                if (!path_bounds[path_index].defined || !path_bounds[path_index].overlap(component_bounds)) continue;
                const ExtrusionPath &path = paths[path_index];
                const Polygons bead = printed_bead(path);
                if (Slic3r::area(intersection(bead, area)) <= min_area)
                    continue;
                if (path.role() == ExtrusionRole::erSupportMaterial)
                    selected.push_back(path);
                else
                    has_interface = true;
                const double bottom = output[idx]->print_z - path.height;
                anchored |= !belt.is_active() && bottom <= object.slicing_parameters().raft_contact_top_z + EPSILON;
                if (belt.is_active())
                    anchored |= touches(bead, belt.surface_polygon(bottom));
                for (size_t lower = idx; lower-- > 0 && plan[lower].print_z >= bottom - EPSILON;)
                    anchored |= touches(bead, footprint(lower));
                if (!anchored)
                    anchored = touches_contact_plane(bead, bottom, output[idx]->print_z);
                if (!anchored && config.support_rests_on_model) {
                    const auto &layers = object.layers();
                    const double below = bottom - object.slicing_parameters().gap_object_support + object_z_offset;
                    auto it = std::upper_bound(layers.begin(), layers.end(), below + EPSILON,
                        [](double z, const Layer *layer) { return z < layer->print_z; });
                    if (it != layers.begin())
                        anchored |= touches(bead, to_polygons((*std::prev(it))->lslices));
                }
                if (anchored)
                    break;
            }
            if (anchored)
                continue;
            if (selected.empty()) {
                // Do not turn an interface into a trunk. Only discard a
                // sub-bead island which has no printed predecessor, bed or
                // model landing; a printable contact platform is retained.
                if (has_interface && component.area() < sqr(double(config.support_line_width))) {
                    remove_fragment(output[idx]->support_fills, area, ExtrusionRole::erSupportMaterialInterface);
                    invalidate_footprints();
                    output[idx]->support_islands = union_ex(footprint(idx));
                    ++removed;
                }
                continue;
            }
            if (trace_anchors) {
                const BoundingBox bounds = get_extents(component);
                BOOST_LOG_TRIVIAL(debug) << "Unanchored extrusion Z=" << output[idx]->print_z <<
                    ", XY=" << unscale<double>(bounds.min.x()) << "," << unscale<double>(bounds.min.y()) <<
                    ".." << unscale<double>(bounds.max.x()) << "," << unscale<double>(bounds.max.y()) <<
                    ", area=" << component.area() / sqr(double(scale_(1.)));
            }
            std::vector<size_t> missing;
            bool landed = false;
            for (size_t lower = idx; lower-- > 0;) {
                throw_on_cancel();
                if (touches(area, footprint(lower))) {
                    landed = true;
                    break;
                }
                Polygons collisions;
                for (LayerIndex source = plan[lower].source_layer_begin; source <= plan[lower].source_layer_end; ++source)
                    append(collisions, volumes.getCollision(0, source, false));
                const bool lands_on_model = model_landing(plan[lower].bottom_z, area);
                if ((touches(area, collisions) && !lands_on_model) || !diff(area, Polygons{volumes.m_bed_area}).empty() ||
                    (belt.is_active() && touches(area, belt.surface_polygon(plan[lower].print_z)))) {
                    if (trace_anchors)
                        BOOST_LOG_TRIVIAL(debug) << "Continuation blocked at Z=" << plan[lower].print_z <<
                            ", model=" << touches(area, collisions) << ", lands_on_model=" << lands_on_model <<
                            ", bed=" << !diff(area, Polygons{volumes.m_bed_area}).empty();
                    break;
                }
                missing.push_back(lower);
                if (lands_on_model) {
                    landed = true;
                    break;
                }
            }
            if (!landed) {
                if (independent_organic) {
                    // A component without a collision-safe route to the bed,
                    // another support, or a model landing is an airborne tree
                    // fragment. Drop it instead of printing a floating trunk;
                    // relaxing the configured clearances would hide the defect.
                    remove_fragment(output[idx]->support_fills, area, ExtrusionRole::erSupportMaterial);
                    invalidate_footprints();
                    output[idx]->support_islands = union_ex(footprint(idx));
                    ++removed;
                } else {
                    // Keep the legacy synchronized-grid behavior for classic
                    // organic output; only discard sub-bead interface debris.
                    if (component.area() < sqr(double(config.support_line_width)) &&
                        std::all_of(selected.begin(), selected.end(), [](const ExtrusionPath &path) {
                            return path.role() == ExtrusionRole::erSupportMaterialInterface;
                        })) {
                        remove_fragment(output[idx]->support_fills, area, ExtrusionRole::erSupportMaterialInterface);
                        invalidate_footprints();
                        output[idx]->support_islands = union_ex(footprint(idx));
                        ++removed;
                    } else {
                        ++unresolved;
                    }
                }
                continue;
            }
            if (missing.empty())
                continue;
            for (size_t lower : missing) {
                const auto &entry = plan[lower];
                if (output[lower] == nullptr)
                    output[lower] = object.add_support_layer(0, 0, entry.height, entry.print_z);
                for (const ExtrusionPath &path : selected) {
                    ExtrusionPath continuation(path);
                    const Flow flow(path.width, float(entry.height), support_params.support_material_flow.nozzle_diameter());
                    continuation.height = flow.height();
                    continuation.mm3_per_mm = flow.mm3_per_mm();
                    continuation.set_extrusion_role(ExtrusionRole::erSupportMaterial);
                    output[lower]->support_fills.append(std::move(continuation));
                }
                output[lower]->support_islands = union_ex(output[lower]->support_islands, area);
                output[lower]->height = std::min(output[lower]->height, entry.height);
                invalidate_footprints();
            }
            ++repaired;
        }
    }
    footprint_cache.clear();
    const auto profile_anchored = std::chrono::steady_clock::now();
    auto &layers = object.support_layers();
    std::sort(layers.begin(), layers.end(), [](const SupportLayer *a, const SupportLayer *b) { return a->print_z < b->print_z; });
    // A fork can introduce a curved inner wall over the empty interior of an
    // otherwise connected hollow trunk. Connectivity of filled source areas
    // alone cannot detect this. Brace that junction on the preceding existing
    // plane, using straight, two-ended native infill inside that same trunk.
    std::vector<const SupportGeneratorLayer *> sections;
    for (const SupportGeneratorLayer *source : structural_layers)
        if (source != nullptr && !source->polygons.empty())
            sections.push_back(source);
    std::sort(sections.begin(), sections.end(), [](const auto *a, const auto *b) { return *a < *b; });
    sections.erase(std::unique(sections.begin(), sections.end()), sections.end());
    auto section_at = [&](double z, bool interfaces) {
        Polygons result;
        auto source = std::lower_bound(sections.begin(), sections.end(), z - EPSILON,
            [](const auto *part, double plane) { return part->print_z < plane; });
        for (; source != sections.end() && (*source)->print_z <= z + EPSILON; ++source)
            if (!interfaces || ((*source)->layer_type != SupporLayerType::Base &&
                                (*source)->layer_type != SupporLayerType::Intermediate))
                append(result, (*source)->polygons);
        return union_(result);
    };
    std::unordered_map<const SupportLayer *, Polygons> layer_cache;
    size_t layer_cache_bytes = 0;
    using FoundationIndex = AABBTreeLines::LinesDistancer<Linef>;
    std::unordered_map<const SupportLayer *, std::unique_ptr<FoundationIndex>> foundation_indexes;
    size_t index_cache_bytes = 0;
    size_t largest_foundation = 0, rejected_foundations = 0, built_foundations = 0;
    auto invalidate_layers = [&] {
        layer_cache.clear();
        layer_cache_bytes = 0;
        foundation_indexes.clear();
        index_cache_bytes = 0;
    };
    auto layer_beads = [&](const SupportLayer &layer) {
        if (layer_cache_limit > 0) {
            const auto found = layer_cache.find(&layer);
            if (found != layer_cache.end()) return found->second;
        }
        Polygons result;
        for_each_extrusion_path(layer.support_fills, [&](const ExtrusionPath &path) {
            if (is_support(path.role()) && !path.is_force_no_extrusion() && path.mm3_per_mm > 0.)
                append(result, printed_bead(path));
        });
        result = union_(result);
        const size_t bytes = polygon_bytes(result);
        if (bytes <= layer_cache_limit) {
            if (layer_cache_bytes + bytes > layer_cache_limit || layer_cache.size() >= 8)
                invalidate_layers();
            try {
                layer_cache.emplace(&layer, result);
                layer_cache_bytes += bytes;
            } catch (const std::bad_alloc &) {
                invalidate_layers();
            }
        }
        return result;
    };
    auto foundation_index = [&](const SupportLayer &layer, const Polygons &polygons) -> const FoundationIndex * {
        if (index_cache_limit == 0) return nullptr;
        const auto found = foundation_indexes.find(&layer);
        if (found != foundation_indexes.end()) return found->second.get();
        size_t vertices = 0;
        for (const Polygon &polygon : polygons) vertices += polygon.size();
        largest_foundation = std::max(largest_foundation, vertices);
        const size_t bytes = vertices * sizeof(Linef) * 16 + 256;
        if (bytes > index_cache_limit) { ++rejected_foundations; return nullptr; }
        try {
            std::vector<Linef> edges;
            edges.reserve(vertices);
            for (const Polygon &polygon : polygons)
                for (size_t i = 0; i < polygon.size(); ++i) {
                    const Point &a = polygon.points[i], &b = polygon.points[(i + 1) % polygon.size()];
                    if (a != b) edges.emplace_back(a.cast<double>(), b.cast<double>());
                }
            if (edges.empty()) return nullptr;
            if (index_cache_bytes + bytes > index_cache_limit || foundation_indexes.size() >= 8) {
                foundation_indexes.clear();
                index_cache_bytes = 0;
            }
            auto index = std::make_unique<FoundationIndex>(std::move(edges));
            ++built_foundations;
            const FoundationIndex *result = index.get();
            foundation_indexes.emplace(&layer, std::move(index));
            index_cache_bytes += bytes;
            return result;
        } catch (const std::bad_alloc &) {
            foundation_indexes.clear();
            index_cache_bytes = 0;
            return nullptr;
        }
    };
    auto model_bearing = [&](const Polygons &bead, double bottom, Polygons &bearing) {
        if (!config.support_rests_on_model)
            return;
        const double landing = bottom - object.slicing_parameters().gap_object_support + object_z_offset;
        // A lower model slice cannot stand in for an absent printed support
        // plane. Use the actual (possibly grid-rounded) configured gap.
        const double rounding = EPSILON;
        const auto &model_layers = object.layers();
        auto model = std::lower_bound(model_layers.begin(), model_layers.end(), landing - rounding - EPSILON,
            [](const Layer *part, double z) { return part->print_z < z; });
        for (; model != model_layers.end() && (*model)->print_z <= landing + EPSILON; ++model)
            append(bearing, intersection(bead, to_polygons((*model)->lslices)));
    };
    double max_height = 0.;
    for (const SupportLayer *layer : layers)
        for_each_extrusion_path(layer->support_fills, [&](const ExtrusionPath &path) {
            max_height = std::max(max_height, double(path.height));
        });
    std::deque<std::pair<size_t, Polygons>> recent;
    std::set<const ExtrusionEntity *> joined_interfaces;
    // Join interface finishes before checking upper-road bearings, so any
    // required local brace also sees the final native connector geometry.
    for (SupportLayer *layer : layers) {
        if (layer->bottom_z() <= object.slicing_parameters().raft_contact_top_z + EPSILON)
            continue;
        std::map<float, Polygons> interfaces;
        for (const SupportGeneratorLayer *source : sections) {
            if (std::abs(source->print_z - layer->print_z) > EPSILON ||
                (source->layer_type != SupporLayerType::TopContact &&
                 source->layer_type != SupporLayerType::BottomContact &&
                 source->layer_type != SupporLayerType::TopInterface &&
                 source->layer_type != SupporLayerType::BottomInterface))
                continue;
            Polygons domain = source->polygons;
            if (source->layer_type == SupporLayerType::BottomContact) {
                // A root-wall neck may be thinner than one bead. Permit a
                // local bridge inside a one-bead collar, with the same physical
                // contact slab and the original model/XY/blocker clearances.
                domain = diff(offset(domain, scale_(support_params.support_material_interface_flow.width())),
                    organic_support_slab_collision(object, volumes, config, support_params,
                        source->bottom_print_z(), source->print_z));
                if (belt.is_active())
                    domain = diff(domain, belt.surface_polygon(source->print_z));
                for_each_extrusion_path(layer->support_fills, [&](const ExtrusionPath &path) {
                    if (path.role() == erSupportMaterialInterface &&
                        std::abs(path.height - source->height) <= EPSILON &&
                        !intersection_pl(Polylines{path.polyline.to_polyline()}, source->polygons).empty())
                        append(domain, printed_bead(path));
                });
            }
            append(interfaces[float(source->height)], std::move(domain));
        }
        if (interfaces.empty()) continue;
        size_t joined = 0;
        ExtrusionPaths upper_paths;
        Polygons future_keepout;
        const double minimum_height = object.slicing_parameters().min_layer_height;
        for (const SupportLayer *below : layers)
            if (below->print_z < layer->print_z - EPSILON &&
                layer->print_z - below->print_z < minimum_height - EPSILON)
                append(future_keepout, layer_beads(*below));
        for (const SupportLayer *upper : layers) {
            if (upper->print_z <= layer->print_z + EPSILON) continue;
            if (upper->print_z > layer->print_z + max_height + EPSILON) break;
            for_each_extrusion_path(upper->support_fills, [&](const ExtrusionPath &path) {
                if (is_support(path.role()) && path.mm3_per_mm > 0. &&
                    std::abs(upper->print_z - path.height - layer->print_z) <= EPSILON)
                    upper_paths.push_back(path);
                else if (is_support(path.role()) && path.mm3_per_mm > 0. &&
                    upper->print_z - path.height < layer->print_z - EPSILON)
                    append(future_keepout, printed_bead(path));
            });
        }
        for (const auto &[height, domains] : interfaces)
            joined += connect_support_interface_paths(layer->support_fills,
                diff_ex(intersection(domains, volumes.m_bed_area), future_keepout),
                height, 8. * scale_(support_params.support_material_interface_flow.width()), upper_paths, &joined_interfaces);
        if (joined > 0) {
            invalidate_layers();
            layer->support_islands = union_ex(layer->support_islands, layer_beads(*layer));
        }
    }
    if (modulate_joined_support_paths(layers, float(object.slicing_parameters().min_layer_height), joined_interfaces) > 0)
        invalidate_layers();
    const auto profile_joined = std::chrono::steady_clock::now();
    size_t braces = 0;
    size_t certified_roads = 0;
    size_t largest_road = 0;
    double certificate_seconds = 0., bearing_seconds = 0., span_seconds = 0.;
    double root_seconds = 0., inspection_seconds = 0., brace_seconds = 0.;
    double direction_seconds = 0., brace_check_seconds = 0.;
    size_t certified_bridges = 0;
    size_t needed_planes = 0, allowed_parts = 0, directed_parts = 0, planned_roads = 0;
    Polygons transitioning;
    double transition_z = -std::numeric_limits<double>::max();
    size_t shaped_roots = 0, discarded_whiskers = 0;
    for (size_t idx = 1; idx < layers.size(); ++idx) {
        throw_on_cancel();
        SupportLayer &lower = *layers[idx - 1];
        while (!recent.empty() && layers[recent.front().first]->print_z < lower.print_z - max_height - EPSILON)
            recent.pop_front();
        if (recent.empty() || recent.back().first != idx - 1)
            recent.emplace_back(idx - 1, layer_beads(lower));
        SupportLayer &current = *layers[idx];
        Polygons bottom_transition;
        for (const auto &[index, below] : recent) {
            const double z = layers[index]->print_z;
            auto source = std::lower_bound(sections.begin(), sections.end(), z - EPSILON,
                [](const auto *part, double plane) { return part->print_z < plane; });
            for (; source != sections.end() && (*source)->print_z <= z + EPSILON; ++source)
                if ((*source)->layer_type == SupporLayerType::BottomContact || (*source)->layer_type == SupporLayerType::BottomInterface)
                    append(bottom_transition, (*source)->polygons);
        }
        if (current.print_z <= transition_z + max_height + EPSILON)
            append(bottom_transition, transitioning);
        Polygons contact_platform;
        bool has_thin_contact = false;
        for_each_extrusion_path(current.support_fills, [&](const ExtrusionPath &path) {
            has_thin_contact |= path.role() == erSupportMaterialInterface &&
                path.width < support_params.support_material_flow.nozzle_diameter() * 0.5;
        });
        if (has_thin_contact) {
            for_each_extrusion_path(current.support_fills, [&](const ExtrusionPath &path) {
                if (path.role() == erSupportMaterialInterface && path.width >= support_params.support_material_flow.nozzle_diameter() * 0.5)
                    append(contact_platform, printed_bead(path));
            });
            contact_platform = offset(union_(contact_platform), scale_(support_params.support_material_flow.nozzle_diameter()));
        }
        Polygons new_transition;
        std::function<void(ExtrusionEntityCollection &)> shape_roots;
        shape_roots = [&](ExtrusionEntityCollection &collection) {
            ExtrusionEntitiesPtr kept;
            for (ExtrusionEntity *entity : collection.entities) {
                if (auto *children = dynamic_cast<ExtrusionEntityCollection *>(entity)) {
                    shape_roots(*children);
                    if (!children->empty())
                        kept.push_back(entity);
                    else
                        delete entity;
                    continue;
                }
                ExtrusionPaths road;
                collect_paths(*entity, road);
                const double nozzle = support_params.support_material_flow.nozzle_diameter();
                const bool thin_contact = !road.empty() && road.front().role() == erSupportMaterialInterface &&
                    std::all_of(road.begin(), road.end(), [&](const ExtrusionPath &path) { return path.width < nozzle * 0.5; });
                if (road.empty() || (!thin_contact &&
                        (road.front().role() != erSupportMaterial || bottom_transition.empty()))) {
                    kept.push_back(entity);
                    continue;
                }
                if (!thin_contact) {
                    // The existing root-shaping rule can only alter one
                    // uniform, closed structural road. Reject its other cases
                    // before building any deposited geometry or bearing masks.
                    bool eligible = road.front().first_point() == road.back().last_point();
                    BoundingBox bounds;
                    for (size_t i = 0; i < road.size(); ++i) {
                        eligible &= std::abs(road[i].width - road.front().width) < EPSILON &&
                            std::abs(road[i].height - road.front().height) < EPSILON &&
                            (i == 0 || road[i - 1].last_point() == road[i].first_point());
                        for (const Point3 &point : road[i].polyline.points) bounds.merge(point.to_point());
                    }
                    if (bounds.defined) bounds.offset(scale_(std::abs(road.front().width)) + 4 * SCALED_EPSILON);
                    if (!eligible || !bounds.defined || !bounds.overlap(get_extents(bottom_transition))) {
                        kept.push_back(entity);
                        continue;
                    }
                }
                Polyline line;
                Polygons bearing, beads;
                double width = std::numeric_limits<double>::max(), max_width = 0.;
                bool uniform = !road.empty();
                for (const ExtrusionPath &path : road) {
                    uniform &= std::abs(path.width - road.front().width) < EPSILON &&
                        std::abs(path.height - road.front().height) < EPSILON;
                    const Polyline part = path.polyline.to_polyline();
                    if (line.empty())
                        line = part;
                    else if (line.last_point() == part.first_point())
                        line.points.insert(line.points.end(), part.points.begin() + 1, part.points.end());
                    else
                        uniform = false;
                    width = std::min(width, double(path.width));
                    max_width = std::max(max_width, double(path.width));
                    const Polygons bead = printed_bead(path);
                    append(beads, bead);
                    const double bottom = current.print_z - path.height;
                    for (const auto &[index, below] : recent)
                        if (layers[index]->print_z >= bottom - EPSILON)
                            append(bearing, intersection(bead, below));
                    model_bearing(bead, bottom, bearing);
                }
                if (road.empty()) {
                    kept.push_back(entity);
                    continue;
                }
                bearing = union_(bearing);
                const double bearing_area = area(bearing);
                if (road.front().role() == erSupportMaterialInterface && max_width < nozzle * 0.5 &&
                    line.length() < scale_(nozzle) &&
                    area(intersection(beads, contact_platform)) > sqr(double(scale_(0.001))) &&
                    bearing_area < std::min(area(union_(beads)) * 0.05, sqr(double(scale_(max_width))) * 0.5)) {
                    delete entity;
                    ++discarded_whiskers;
                    continue;
                }
                if (!uniform || road.front().role() != erSupportMaterial || !line.is_closed() ||
                    area(intersection(beads, bottom_transition)) <= min_area ||
                    organic_support_unbacked_spans(line, bearing, scale_(width * 0.5), scale_(width), scale_(0.002),
                        0., scale_(width * 0.5), !road.front().can_reverse()).empty()) {
                    kept.push_back(entity);
                    continue;
                }
                // A true internal void wall belongs to a fork, not to the
                // flared outside of a model landing. Keep its topology and
                // let the junction-bracing pass supply its missing bearing.
                Polygon enclosed{Points(line.points.begin(), line.points.end() - 1)};
                enclosed.make_counter_clockwise();
                const Polygons interior = offset(Polygons{enclosed}, -scale_(width * 0.5));
                if (area(intersection(interior, section_at(current.print_z, false))) <= min_area) {
                    kept.push_back(entity);
                    continue;
                }
                Polygons foundation;
                for (const auto &[index, below] : recent)
                    if (layers[index]->print_z >= current.print_z - road.front().height - EPSILON)
                        for (const ExPolygon &part : union_ex(below))
                            foundation.push_back(part.contour);
                Polygons allowed = offset(union_(foundation), scale_(width * 0.25), jtRound, scale_(0.001));
                const LayerIndex source_index = plan.find_exact(current.print_z);
                if (source_index < 0) {
                    kept.push_back(entity);
                    continue;
                }
                Polygons collisions;
                for (LayerIndex source = plan[source_index].source_layer_begin; source <= plan[source_index].source_layer_end; ++source)
                    append(collisions, volumes.getCollision(0, source, false));
                allowed = diff(allowed, offset(union_(collisions), scale_(width * 0.5)));
                const Polylines contours = organic_support_closed_contours(line, allowed, scale_(width));
                if (contours.empty()) {
                    kept.push_back(entity);
                    continue;
                }
                for (const Polyline &contour : contours) {
                    auto *path = new ExtrusionPath(road.front());
                    path->polyline = Polyline3(contour);
                    kept.push_back(path);
                    append(new_transition, printed_bead(*path));
                }
                delete entity;
                ++shaped_roots;
            }
            collection.entities = std::move(kept);
        };
        const size_t previous_root_changes = shaped_roots + discarded_whiskers;
        const auto root_started = std::chrono::steady_clock::now();
        shape_roots(current.support_fills);
        root_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - root_started).count();
        if (shaped_roots + discarded_whiskers != previous_root_changes)
            invalidate_layers();
        if (!new_transition.empty()) {
            transitioning = union_(new_transition);
            transition_z = current.print_z;
        }
        // The first layer may also need local bearing rows for a hollow
        // successor. Keep those rows inside the original support section;
        // the later flange/brim stage still owns its external footprint.
        Polygons needs;
        struct BearingSample { Point point; double width; };
        std::vector<BearingSample> bearing_samples;
        const auto inspection_started = std::chrono::steady_clock::now();
        for (size_t future = idx; future < layers.size() && layers[future]->print_z <= lower.print_z + max_height + EPSILON; ++future) {
        const SupportLayer &target = *layers[future];
        bool can_add_bearing = false;
        for_each_extrusion_path(target.support_fills, [&](const ExtrusionPath &path) {
            can_add_bearing |= is_support(path.role()) && !path.is_force_no_extrusion() && path.mm3_per_mm > 0. &&
                target.print_z - path.height <= lower.print_z + EPSILON;
        });
        if (!can_add_bearing) continue;
        Polygons ordered_plane;
        std::function<void(const ExtrusionEntity &)> inspect_road;
        inspect_road = [&](const ExtrusionEntity &entity) {
            if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
                for (const ExtrusionEntity *child : collection->entities)
                    inspect_road(*child);
                return;
            }
            ExtrusionPaths road;
            collect_paths(entity, road);
            if (road.empty())
                return;
            Polyline line;
            Polygons bearing, road_beads;
            double width = std::numeric_limits<double>::max();
            double max_width = 0.;
            double latest_bottom = -std::numeric_limits<double>::max();
            bool grounded = false, repairable = true;
            for (const ExtrusionPath &path : road) {
                const Polyline part = path.polyline.to_polyline();
                if (line.empty())
                    line = part;
                else if (line.last_point() == part.first_point())
                    line.points.insert(line.points.end(), part.points.begin() + 1, part.points.end());
                else
                    return;
                width = std::min(width, double(path.width));
                max_width = std::max(max_width, double(path.width));
                const double bottom = target.print_z - path.height;
                latest_bottom = std::max(latest_bottom, bottom);
                grounded |= !belt.is_active() && bottom <= object.slicing_parameters().raft_contact_top_z + EPSILON;
                repairable &= bottom <= lower.print_z + EPSILON;
                const Polygons bead = printed_bead(path);
                append(road_beads, bead);
            }
            road_beads = union_(road_beads);
            if (grounded) {
                append(ordered_plane, road_beads);
                return;
            }
            const double road_area = area(road_beads);
            if (road_area < sqr(double(scale_(support_params.support_material_flow.nozzle_diameter()))) * 0.05)
                return;
            const double required_bearing = std::min(road_area * 0.05, sqr(double(scale_(max_width))) * 0.5);
            largest_road = std::max(largest_road, line.size());
            const auto certificate_started = std::chrono::steady_clock::now();
            auto definitely_backed = [&](const SupportLayer &below, const Polygons &material) {
                if (below.print_z < latest_bottom - EPSILON) return false;
                const FoundationIndex *index = foundation_index(below, material);
                return index != nullptr && organic_support_definitely_backed(line, *index, scale_(width), required_bearing);
            };
            bool certified = false;
            for (const auto &[index, below] : recent)
                if (definitely_backed(*layers[index], below)) { certified = true; break; }
            for (size_t between = idx; !certified && between < future; ++between)
                if (layers[between]->print_z >= latest_bottom - EPSILON)
                    certified = definitely_backed(*layers[between], layer_beads(*layers[between]));
            certificate_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - certificate_started).count();
            if (certified) {
                append(ordered_plane, road_beads);
                ++certified_roads;
                return;
            }
            const auto bearing_started = std::chrono::steady_clock::now();
            for (const ExtrusionPath &path : road) {
                const Polygons bead = printed_bead(path);
                const double bottom = target.print_z - path.height;
                for (const auto &[index, below] : recent)
                    if (layers[index]->print_z >= bottom - EPSILON)
                        append(bearing, intersection(bead, below));
                for (size_t between = idx; between < future; ++between)
                    if (layers[between]->print_z >= bottom - EPSILON)
                        append(bearing, intersection(bead, layer_beads(*layers[between])));
                model_bearing(bead, bottom, bearing);
            }
            const bool structural_wall = road.front().role() == erSupportMaterial &&
                !road.front().can_reverse() && line.is_closed();
            // Closed trunk walls need their predecessor below. A same-plane
            // interface connector may join two supported ends, but it must
            // not make a new hollow trunk wall appear vertically supported.
            if (!structural_wall)
                append(bearing, intersection(road_beads, union_(ordered_plane)));
            bearing = union_(bearing);
            bearing_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - bearing_started).count();
            const auto span_started = std::chrono::steady_clock::now();
            const Polylines unsupported = organic_support_unbacked_spans(line, bearing,
                    scale_(width * 0.5), scale_(width), scale_(0.002), 0., scale_(width * 0.5),
                    structural_wall);
            span_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - span_started).count();
            if (unsupported.empty() && area(bearing) + sqr(double(scale_(0.001))) >=
                    std::min(road_area * 0.05, sqr(double(scale_(max_width))) * 0.5))
                append(ordered_plane, road_beads);
            else if (repairable)
                for (const Polyline &span : unsupported) {
                    Polyline simplified = span;
                    simplified.simplify(scale_(std::max(0.002, width * 0.01)));
                    Polygons bends;
                    if (road.front().role() != erSupportMaterial || road.front().can_reverse())
                        for (size_t vertex = 1; vertex + 1 < simplified.size(); ++vertex)
                            if (line_alg::distance_to_squared(Line(simplified[vertex - 1], simplified[vertex + 1]),
                                    simplified[vertex]) > sqr(double(scale_(0.002)))) {
                                append(bends, offset(Polyline{Points{simplified[vertex], simplified[vertex]}},
                                    float(scale_(width)), jtRound, scale_(0.001), etOpenRound));
                                bearing_samples.push_back({simplified[vertex], scale_(width)});
                            }
                    if (bends.empty()) {
                        append(needs, offset(span, float(scale_(width)), jtRound, scale_(0.001), etOpenRound));
                        for (const Point &point : span.equally_spaced_points(scale_(width * 0.25)))
                            bearing_samples.push_back({point, scale_(width)});
                        bearing_samples.push_back({span.last_point(), scale_(width)});
                    } else
                        append(needs, std::move(bends));
                }
        };
        inspect_road(target.support_fills);
        }
        inspection_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - inspection_started).count();
        if (needs.empty())
            continue;
        ++needed_planes;
        const auto brace_started = std::chrono::steady_clock::now();
        Polygons forbidden = section_at(lower.print_z, true);
        const LayerIndex source_index = plan.find_exact(lower.print_z);
        if (source_index < 0)
            continue;
        if (independent_organic)
            append(forbidden, organic_support_slab_collision(object, volumes, config, support_params,
                lower.bottom_z(), lower.print_z));
        else
            for (LayerIndex source = plan[source_index].source_layer_begin; source <= plan[source_index].source_layer_end; ++source)
                append(forbidden, volumes.getCollision(0, source, false));
        if (belt.is_active())
            append(forbidden, belt.surface_polygon(lower.print_z));
        const Flow flow(support_params.support_material_flow.width(), float(lower.height),
            support_params.support_material_flow.nozzle_diameter());
        std::optional<Polygons> inset_foundation;
        std::optional<ExPolygons> bridge_anchors;
        const FoundationIndex *brace_foundation = foundation_index(lower, recent.back().second);
        const size_t previous_braces = braces;
        for (const ExPolygon &part : union_ex(section_at(lower.print_z, false))) {
            // Never bridge separate roots or a model keepout. Source holes
            // representing empty trunk interiors may receive a local brace.
            const Polygons allowed = intersection(diff(Polygons{part.contour}, forbidden), volumes.m_bed_area);
            const Polygons demand = intersection(needs, allowed);
            if (demand.empty())
                continue;
            ++allowed_parts;
            if (!bridge_anchors) bridge_anchors = union_ex(recent.back().second);
            const auto direction_started = std::chrono::steady_clock::now();
            const ExPolygons air = diff_ex(allowed, recent.back().second);
            BridgeDetector bridge(air, *bridge_anchors, scale_(flow.width()));
            double angle = bridge.angle;
            if (bridge.detect_angle())
                angle = bridge.angle;
            else {
                const auto [axis, distance] = detect_bridging_direction(to_polygons(air), recent.back().second);
                angle = std::atan2(axis.y(), axis.x());
            }
            direction_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - direction_started).count();
            ++directed_parts;
            // Limit bracing to the perpendicular extent of the unsupported
            // curve; the parallel extent reaches the original trunk walls.
            const Vec2d direction(std::cos(angle), std::sin(angle));
            const Vec2d normal(-direction.y(), direction.x());
            double along_min = std::numeric_limits<double>::max(), along_max = -along_min;
            double across_min = along_min, across_max = -along_min;
            for (const Polygon &poly : allowed)
                for (const Point &point : poly.points) {
                    const double along = point.cast<double>().dot(direction);
                    along_min = std::min(along_min, along);
                    along_max = std::max(along_max, along);
                }
            Polygons bands;
            for (const ExPolygon &patch : union_ex(demand)) {
                across_min = std::numeric_limits<double>::max();
                across_max = -across_min;
                for (const Point &point : patch.contour.points) {
                    const double across = point.cast<double>().dot(normal);
                    across_min = std::min(across_min, across);
                    across_max = std::max(across_max, across);
                }
                Polygon band;
                for (const Vec2d &p : {direction * along_min + normal * across_min,
                        direction * along_max + normal * across_min, direction * along_max + normal * across_max,
                        direction * along_min + normal * across_max})
                    band.points.push_back(p.cast<coord_t>());
                bands.push_back(std::move(band));
            }
            std::unique_ptr<Fill> fill(Fill::new_from_type(ipRectilinear));
            fill->spacing = flow.spacing();
            // Fill normally insets by half its spacing. Reserve half the
            // actual bead width at collisions, interfaces and bed edges.
            fill->overlap = -0.5 * (flow.width() - flow.spacing());
            fill->angle = float(angle);
            fill->fixed_angle = true;
            fill->set_bounding_box(get_extents(part));
            FillParams params;
            params.density = 1.f;
            params.anchor_length = params.anchor_length_max = 0.f;
            ExtrusionPaths candidates;
            for (const ExPolygon &region : intersection_ex(allowed, union_(bands))) {
                const Surface surface(stInternal, region);
                for (Polyline &line : fill->fill_surface(&surface, params)) {
                    ++planned_roads;
                    // The predecessor stays unchanged across every component
                    // and candidate in this plane. Preserve the exact native
                    // inset while computing it only when the first road needs it.
                    if (!inset_foundation)
                        inset_foundation = offset(recent.back().second, -scale_(flow.width() * 0.25));
                    for (Polyline &extra : diff_pl(Polylines{line}, *inset_foundation)) {
                        if (extra.length() < scale_(flow.width()))
                            continue;
                        // The bed is the foundation of a first-layer brace.
                        // Requiring another printed road below it rejects the
                        // very flange rows needed by the next hollow trunk.
                        const bool on_bed = !belt.is_active() &&
                            lower.bottom_z() <= object.slicing_parameters().raft_contact_top_z + EPSILON;
                        const auto check_started = std::chrono::steady_clock::now();
                        const bool certified = brace_foundation != nullptr &&
                            organic_support_definitely_bridged(extra, *brace_foundation, scale_(0.002));
                        certified_bridges += certified;
                        const bool printable = on_bed || certified || organic_support_unbacked_spans(extra,
                            recent.back().second, scale_(flow.width() * 0.5), scale_(flow.width()), scale_(0.002)).empty();
                        brace_check_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - check_started).count();
                        if (!printable)
                            continue;
                        ExtrusionPath brace(erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height());
                        brace.polyline = Polyline3(std::move(extra));
                        candidates.push_back(std::move(brace));
                    }
                }
            }
            // Keep only the few native bridge roads needed by the future
            // bends. Straight spans between those bearings remain bridges;
            // they do not require a dense plate across the whole trunk.
            std::vector<std::vector<size_t>> coverage(candidates.size());
            for (size_t candidate = 0; candidate < candidates.size(); ++candidate) {
                const Polyline line = candidates[candidate].polyline.to_polyline();
                BoundingBox bounds = get_extents(line);
                bounds.offset(scale_(flow.width()));
                for (size_t sample = 0; sample < bearing_samples.size(); ++sample) {
                    const auto &[point, width] = bearing_samples[sample];
                    if (!bounds.contains(point))
                        continue;
                    const double reach = 0.5 * (scale_(flow.width()) + width) - scale_(0.002);
                    for (size_t segment = 1; segment < line.size(); ++segment)
                        if (line_alg::distance_to_squared(Line(line[segment - 1], line[segment]), point) <= sqr(reach)) {
                            coverage[candidate].push_back(sample);
                            break;
                        }
                }
            }
            std::vector<bool> covered(bearing_samples.size(), false), selected(candidates.size(), false);
            for (;;) {
                size_t best = candidates.size(), best_gain = 0;
                double best_length = std::numeric_limits<double>::max();
                for (size_t candidate = 0; candidate < candidates.size(); ++candidate) {
                    if (selected[candidate])
                        continue;
                    const size_t gain = std::count_if(coverage[candidate].begin(), coverage[candidate].end(),
                        [&](size_t sample) { return !covered[sample]; });
                    const double length = candidates[candidate].length();
                    if (gain > best_gain || (gain > 0 && gain == best_gain && length < best_length)) {
                        best = candidate;
                        best_gain = gain;
                        best_length = length;
                    }
                }
                if (best == candidates.size())
                    break;
                selected[best] = true;
                for (size_t sample : coverage[best])
                    covered[sample] = true;
            }
            for (size_t candidate = 0; candidate < candidates.size(); ++candidate)
                if (selected[candidate]) {
                    lower.support_fills.append(std::move(candidates[candidate]));
                    ++braces;
                }
        }
        if (braces != previous_braces)
            invalidate_layers();
        recent.back().second = layer_beads(lower);
        brace_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - brace_started).count();
    }
    BOOST_LOG_TRIVIAL(info) << "Organic path validation: anchoring " <<
        std::chrono::duration<double>(profile_anchored - profile_started).count() << " seconds, joining " <<
        std::chrono::duration<double>(profile_joined - profile_anchored).count() << " seconds, bracing " <<
        std::chrono::duration<double>(std::chrono::steady_clock::now() - profile_joined).count() <<
        " seconds, certified roads=" << certified_roads << ", footprint hits=" << footprint_hits;
    BOOST_LOG_TRIVIAL(info) << "Organic bearing details: certificates " << certificate_seconds <<
        " seconds, native bearing " << bearing_seconds << " seconds, spans " << span_seconds <<
        " seconds, largest road=" << largest_road << ", largest foundation=" << largest_foundation <<
        ", indexes=" << built_foundations << ", skipped indexes=" << rejected_foundations;
    BOOST_LOG_TRIVIAL(info) << "Organic repair details: roots " << root_seconds << " seconds, inspection " <<
        inspection_seconds << " seconds, braces " << brace_seconds << " seconds";
    BOOST_LOG_TRIVIAL(info) << "Organic brace details: direction " << direction_seconds << " seconds, checking " <<
        brace_check_seconds << " seconds, certified bridges=" << certified_bridges;
    BOOST_LOG_TRIVIAL(debug) << "Organic junction bracing: " << braces << " straight roads, needed=" << needed_planes <<
        ", sections=" << sections.size() << ", allowed=" << allowed_parts << ", directed=" << directed_parts << ", planned=" << planned_roads <<
        ", root transitions=" << shaped_roots << ", thin contact remnants=" << discarded_whiskers;
    for (size_t idx = 0; idx < layers.size(); ++idx) {
        layers[idx]->set_id(idx);
        layers[idx]->support_fills.no_sort = true;
    }
    BOOST_LOG_TRIVIAL(debug) << "Organic support path anchoring: reconnected " << repaired <<
        " hollow-wall components, removed " << removed << " contact fragments, remaining candidates " << unresolved << ", output planes=" << plan.size() <<
        ", final Z=" << plan[plan.size() - 1].print_z;
}

// Organic specific: Smooth branches and produce one cumulative mesh to be sliced.
OrganicSupportLayerPlan organic_draw_branches(
    PrintObject                     &print_object,
    TreeModelVolumes                &volumes, 
    const TreeSupportSettings       &config,
    std::vector<SupportElements>    &move_bounds,

    // I/O:
    SupportGeneratorLayersPtr       &bottom_contacts,
    SupportGeneratorLayersPtr       &top_contacts,
    InterfacePlacer                 &interface_placer,

    // Output:
    SupportGeneratorLayersPtr       &intermediate_layers,
    SupportGeneratorLayerStorage    &layer_storage,

    std::function<void()>            throw_on_cancel,
    bool                             *source_repaired)
{
    const auto profile_start = std::chrono::steady_clock::now();
    trace_organic_timing("draw.linearization.begin");
    // All SupportElements are put into a layer independent storage to improve parallelization.
    std::vector<std::pair<SupportElement*, int>> elements_with_link_down;
    std::vector<size_t>                          linear_data_layers;
    {
        std::vector<std::pair<SupportElement*, int>> map_downwards_old;
        std::vector<std::pair<SupportElement*, int>> map_downwards_new;
        linear_data_layers.emplace_back(0);
        for (LayerIndex layer_idx = 0; layer_idx < LayerIndex(move_bounds.size()); ++ layer_idx) {
            SupportElements *layer_above = layer_idx + 1 < LayerIndex(move_bounds.size()) ? &move_bounds[layer_idx + 1] : nullptr;
            map_downwards_new.clear();
            std::sort(map_downwards_old.begin(), map_downwards_old.end(), [](auto& l, auto& r) { return l.first < r.first;  });
            SupportElements &layer = move_bounds[layer_idx];
            for (size_t elem_idx = 0; elem_idx < layer.size(); ++ elem_idx) {
                SupportElement &elem = layer[elem_idx];
                int child = -1;
                if (layer_idx > 0) {
                    auto it = std::lower_bound(map_downwards_old.begin(), map_downwards_old.end(), &elem, [](auto& l, const SupportElement* r) { return l.first < r; });
                    if (it != map_downwards_old.end() && it->first == &elem) {
                        child = it->second;
                        // Only one link points to a node above from below.
                        assert(!(++it != map_downwards_old.end() && it->first == &elem));
                    }
#ifndef NDEBUG
                    {
                        const SupportElement *pchild = child == -1 ? nullptr : &move_bounds[layer_idx - 1][child];
                        assert(pchild ? pchild->state.result_on_layer_is_set() : elem.state.target_height > layer_idx);
                    }
#endif // NDEBUG
                }
                for (int32_t parent_idx : elem.parents) {
                    SupportElement &parent = (*layer_above)[parent_idx];
                    if (parent.state.result_on_layer_is_set())
                        map_downwards_new.emplace_back(&parent, elem_idx);
                }

                elements_with_link_down.push_back({ &elem, int(child) });
            }
            std::swap(map_downwards_old, map_downwards_new);
            linear_data_layers.emplace_back(elements_with_link_down.size());
        }
    }

    trace_organic_timing("draw.linearization.end");

    throw_on_cancel();

    trace_organic_timing("draw.smoothing.begin");
    organic_smooth_branches_avoid_collisions(print_object, volumes, config, move_bounds, elements_with_link_down, linear_data_layers, throw_on_cancel);
    trace_organic_timing("draw.smoothing.end");
    const auto profile_smoothed = std::chrono::steady_clock::now();
    BOOST_LOG_TRIVIAL(info) << "Organic draw profiling: smoothing " << std::chrono::duration<double>(profile_smoothed - profile_start).count() << " seconds";

    // Reduce memory footprint. After this point only finalize_interface_and_support_areas() will use volumes and from that only collisions with zero radius will be used.
    volumes.clear_all_but_object_collision();

    // Unmark all nodes.
    for (SupportElements &elements : move_bounds)
        for (SupportElement &element : elements)
            element.state.marked = false;

    // Traverse all nodes, generate tubes.
    // Traversal stack with nodes and their current parent

    struct Branch {
        std::vector<const SupportElement*> path;
        bool                               has_root{ false };
        bool                               has_tip { false };
    };

    struct Slice {
        Polygons polygons;
        Polygons bottom_contacts;
        size_t   num_branches{ 0 };
    };

    struct Tree {
        std::vector<Branch>  branches;

        std::vector<Slice>   slices;
        LayerIndex           first_layer_id{ -1 };
    };

    std::vector<Tree>        trees;

    struct TreeVisitor {
        static void visit_recursive(std::vector<SupportElements> &move_bounds, SupportElement &start_element, Tree &out) {
            assert(! start_element.state.marked && ! start_element.parents.empty());
            // Collect elements up to a bifurcation above.
            start_element.state.marked = true;
            // For each branch bifurcating from this point:
            //SupportElements &layer       = move_bounds[start_element.state.layer_idx];
            SupportElements &layer_above = move_bounds[start_element.state.layer_idx + 1];
            bool root = out.branches.empty();
            for (size_t parent_idx = 0; parent_idx < start_element.parents.size(); ++ parent_idx) {
                Branch branch;
                branch.path.emplace_back(&start_element);
                // Traverse each branch until it branches again.
                SupportElement &first_parent = layer_above[start_element.parents[parent_idx]];
                assert(! first_parent.state.marked);
                assert(branch.path.back()->state.layer_idx + 1 == first_parent.state.layer_idx);
                branch.path.emplace_back(&first_parent);
                if (first_parent.parents.size() < 2)
                    first_parent.state.marked = true;
                SupportElement *next_branch = nullptr;
                if (first_parent.parents.size() == 1) {
                    for (SupportElement *parent = &first_parent;;) {
                        assert(parent->state.marked);
                        SupportElement &next_parent = move_bounds[parent->state.layer_idx + 1][parent->parents.front()];
                        assert(! next_parent.state.marked);
                        assert(branch.path.back()->state.layer_idx + 1 == next_parent.state.layer_idx);
                        branch.path.emplace_back(&next_parent);
                        if (next_parent.parents.size() > 1) {
                            // Branching point was reached.
                            next_branch = &next_parent;
                            break;
                        }
                        next_parent.state.marked = true;
                        if (next_parent.parents.size() == 0)
                            // Tip is reached.
                            break;
                        parent = &next_parent;
                    }
                } else if (first_parent.parents.size() > 1)
                    // Branching point was reached.
                    next_branch = &first_parent;
                assert(branch.path.size() >= 2);
                assert(next_branch == nullptr || ! next_branch->state.marked);
                branch.has_root = root;
                branch.has_tip  = ! next_branch;
                out.branches.emplace_back(std::move(branch));
                if (next_branch)
                    visit_recursive(move_bounds, *next_branch, out);
            }
        }
    };

    trace_organic_timing("draw.tree_traversal.begin");
    for (LayerIndex layer_idx = 0; layer_idx + 1 < LayerIndex(move_bounds.size()); ++ layer_idx) {
//        int ielement;
        for (SupportElement& start_element : move_bounds[layer_idx]) {
            if (!start_element.state.marked && !start_element.parents.empty()) {
#if 0
                int found = 0;
                if (layer_idx > 0) {
                    for (auto& el : move_bounds[layer_idx - 1]) {
                        for (auto iparent : el.parents)
                            if (iparent == ielement)
                                ++found;
                    }
                    if (found != 0)
                        printf("Found: %d\n", found);
                }
#endif
                trees.push_back({});
                TreeVisitor::visit_recursive(move_bounds, start_element, trees.back());
                assert(!trees.back().branches.empty());
                //FIXME debugging
#if 0
                if (start_element.state.lost) {
                }
                else if (start_element.state.verylost) {
                } else
                    trees.pop_back();
#endif
            }
//            ++ ielement;
        }
    }
    trace_organic_timing("draw.tree_traversal.end");

    const SlicingParameters &slicing_params = print_object.slicing_parameters();
    const SupportParameters &support_params = interface_placer.support_parameters;
    if (const char *path = std::getenv("INLONG_ORGANIC_STAGE_TRACE")) {
        std::ofstream out(path, std::ios::app);
        out << std::setprecision(15) << "{\"stage\":\"nodes\",\"trees\":[";
        for (size_t tree_id = 0; tree_id < trees.size(); ++tree_id) {
            if (tree_id) out << ',';
            out << '[';
            const Tree &tree = trees[tree_id];
            for (size_t branch_id = 0; branch_id < tree.branches.size(); ++branch_id) {
                if (branch_id) out << ',';
                const Branch &branch = tree.branches[branch_id];
                out << "{\"root\":" << branch.has_root << ",\"tip\":" << branch.has_tip << ",\"points\":[";
                for (size_t i = 0; i < branch.path.size(); ++i) {
                    if (i) out << ',';
                    const SupportElement &el = *branch.path[i];
                    out << '[' << unscale<double>(el.state.result_on_layer.x()) << ','
                        << unscale<double>(el.state.result_on_layer.y()) << ','
                        << layer_z(slicing_params, config, el.state.layer_idx) << ','
                        << unscale<double>(support_element_radius(config, el)) << ','
                        << el.parents.size() << ',' << el.state.target_height << ']';
                }
                out << "]}";
            }
            out << ']';
        }
        out << "]}\n";
    }
    // Branch nodes end below their roofs. A synchronized plan still needs the
    // contact/interface planes above the last node, including sparse trees
    // whose final roof extends past every other branch.
    size_t source_layer_count = move_bounds.size();
    for (const SupportGeneratorLayersPtr *layers :
         {&top_contacts, &interface_placer.top_interfaces_mutable(), &interface_placer.top_base_interfaces_mutable()})
        for (size_t idx = layers->size(); idx-- > source_layer_count;)
            if ((*layers)[idx] != nullptr && !(*layers)[idx]->polygons.empty()) {
                source_layer_count = idx + 1;
                break;
            }
    OrganicSupportLayerPlan layer_plan = OrganicSupportLayerPlan::legacy(slicing_params, config, source_layer_count);

    std::vector<LayerIndex> mandatory_source_layers;
    std::vector<coordf_t> bottom_clearance_planes;
    for (const Tree &tree : trees) {
        for (const Branch &branch : tree.branches) {
            if (branch.has_root)
                mandatory_source_layers.push_back(branch.path.front()->state.layer_idx);
            if (branch.has_root && support_params.organic_independent_layer_height &&
                config.support_rests_on_model && slicing_params.gap_object_support > EPSILON &&
                volumes.m_belt_floor.empty()) {
                const SupportElement &root = *branch.path.front();
                if (root.state.layer_idx > 0 && !root.state.to_buildplate && root.state.result_on_layer_is_set()) {
                    const coordf_t root_z = layer_z(slicing_params, config, root.state.layer_idx);
                    const coord_t radius = support_element_radius(config, root);
                    // A clearance envelope can graze a side wall far above
                    // the platform carrying this root. Use its bearing area,
                    // consistently with bottom-contact normalization below.
                    Polygon reach = make_circle(radius, SUPPORT_TREE_CIRCLE_RESOLUTION);
                    reach.translate(root.state.result_on_layer);
                    const double z_offset = print_object.belt_global_z_offset();
                    const coordf_t lowest = root_z - unscaled<coordf_t>(5 * radius) + z_offset;
                    const auto &layers = print_object.layers();
                    auto layer = std::lower_bound(layers.begin(), layers.end(), lowest,
                        [](const Layer *entry, coordf_t z) { return entry->print_z < z; });
                    const Layer *contact_layer = nullptr;
                    double best_overlap = 0.;
                    const double overlap_tolerance = sqr(double(scale_(0.001)));
                    for (; layer != layers.end(); ++layer) {
                        const coordf_t boundary = (*layer)->print_z - z_offset + slicing_params.gap_object_support;
                        if (boundary >= root_z - slicing_params.min_layer_height + EPSILON)
                            break;
                        const double overlap = area(intersection(Polygons{reach}, to_polygons((*layer)->lslices)));
                        if (overlap > best_overlap + overlap_tolerance ||
                            (overlap > overlap_tolerance && std::abs(overlap - best_overlap) <= overlap_tolerance)) {
                            contact_layer = *layer;
                            best_overlap = overlap;
                        }
                    }
                    if (contact_layer != nullptr)
                        bottom_clearance_planes.push_back(contact_layer->print_z - z_offset + slicing_params.gap_object_support);
                }
            }
            if (branch.has_tip)
                mandatory_source_layers.push_back(branch.path.back()->state.layer_idx);
        }
    }
    const bool requested_independent_output =
        support_params.organic_independent_layer_height ||
        support_params.independent_top_contact_layer_height;
    std::vector<bool> rejected_contacts(top_contacts.size(), false);
    for (size_t idx = 0; idx < top_contacts.size(); ++idx)
        rejected_contacts[idx] = top_contacts[idx] != nullptr && top_contacts[idx]->polygons.empty() &&
            top_contacts[idx]->idx_object_layer_above != size_t(-1);
    bool use_independent_output = requested_independent_output;
    bool use_exact_top_contact  = support_params.independent_top_contact_layer_height;
    std::vector<LayerIndex> contact_output_indices(top_contacts.size(), -1);
    std::string fallback_reason;
    if (use_exact_top_contact) {
        for (size_t contact_idx = 0; contact_idx < top_contacts.size(); ++contact_idx) {
            if (contact_idx < config.raft_layers.size())
                continue;
            const SupportGeneratorLayer *contact = top_contacts[contact_idx];
            if (contact == nullptr || contact->polygons.empty())
                continue;
            if (!std::isfinite(contact->print_z) || contact->print_z <= EPSILON) {
                use_exact_top_contact = false;
                use_independent_output = false;
                fallback_reason = "an exact top contact layer has an invalid Z position";
                break;
            }
        }
    }

    if (use_independent_output) {
        const bool preserve_source_grid = !support_params.organic_independent_layer_height;
        if (std::optional<OrganicSupportLayerPlan> independent_plan = OrganicSupportLayerPlan::independent(
                slicing_params, config, source_layer_count, top_contacts, mandatory_source_layers,
                preserve_source_grid, fallback_reason, bottom_clearance_planes)) {
            OrganicSupportLayerPlan candidate = std::move(*independent_plan);
            for (size_t idx = 0; idx < top_contacts.size(); ++idx)
                if (top_contacts[idx] != nullptr)
                    contact_output_indices[idx] = candidate.find_exact(top_contacts[idx]->print_z);
            SupportGeneratorLayersPtr &top_interfaces = interface_placer.top_interfaces_mutable();
            SupportGeneratorLayersPtr &top_base_interfaces = interface_placer.top_base_interfaces_mutable();
            const bool remapped =
                remap_generator_layers(top_contacts, candidate, true) &&
                remap_generator_layers(top_interfaces, candidate, false) &&
                remap_generator_layers(top_base_interfaces, candidate, false);
            if (remapped) {
                layer_plan = std::move(candidate);
                if (!bottom_contacts.empty())
                    bottom_contacts.assign(layer_plan.size(), nullptr);
                intermediate_layers.assign(layer_plan.size(), nullptr);
                interface_placer.set_layer_initializer([&layer_plan](SupportGeneratorLayer &layer, size_t layer_idx) {
                    layer_plan.initialize(layer, layer_idx);
                });
            } else {
                use_independent_output = false;
                fallback_reason = "contact or interface layers could not be mapped to the independent output plan";
            }
        } else {
            use_independent_output = false;
        }
    }

    if ((requested_independent_output && !use_independent_output) ||
        (support_params.independent_top_contact_layer_height && !use_exact_top_contact)) {
        restore_generator_layers_to_topology(top_contacts, slicing_params, config);
        restore_generator_layers_to_topology(interface_placer.top_interfaces_mutable(), slicing_params, config);
        restore_generator_layers_to_topology(interface_placer.top_base_interfaces_mutable(), slicing_params, config);
        interface_placer.use_legacy_contact_layers();
        layer_plan = OrganicSupportLayerPlan::legacy(slicing_params, config, source_layer_count);
        if (!bottom_contacts.empty())
            bottom_contacts.assign(source_layer_count, nullptr);
        intermediate_layers.assign(source_layer_count, nullptr);
        BOOST_LOG_TRIVIAL(warning) << "Organic independent layer planning could not satisfy the configured layer-height constraints; using the legacy synchronized layer plan. Reason: "
                                   << (fallback_reason.empty() ? "unspecified planning failure" : fallback_reason);
    }

    for (InterfacePlacer::PendingInterface &roof : interface_placer.take_pending_interfaces()) {
        const size_t source_contact_idx = roof.layer_idx + roof.dtt_roof;
        if (source_contact_idx < rejected_contacts.size() && rejected_contacts[source_contact_idx])
            continue;
        LayerIndex output_idx = LayerIndex(roof.layer_idx);
        if (layer_plan.independent()) {
            const size_t contact_idx = roof.layer_idx + roof.dtt_roof;
            if (contact_idx < contact_output_indices.size() && contact_output_indices[contact_idx] >= 0)
                output_idx = std::max<LayerIndex>(0, contact_output_indices[contact_idx] - LayerIndex(roof.dtt_roof));
            else
                output_idx = layer_plan.at_or_below(layer_z(slicing_params, config, roof.layer_idx));
        }
        interface_placer.add_roof(std::move(roof.polygons), size_t(output_idx), roof.dtt_roof);
    }

    std::vector<Polygons> output_collisions;
    if (layer_plan.independent()) {
        output_collisions.resize(layer_plan.size());
        tbb::parallel_for(tbb::blocked_range<size_t>(0, layer_plan.size()),
            [&output_collisions, &layer_plan, &volumes](const tbb::blocked_range<size_t> &range) {
                for (size_t output_idx = range.begin(); output_idx < range.end(); ++output_idx) {
                    const OrganicSupportLayerPlanEntry &entry = layer_plan[output_idx];
                    Polygons collisions;
                    for (LayerIndex source_idx = entry.source_layer_begin; source_idx <= entry.source_layer_end; ++source_idx)
                        append(collisions, volumes.getCollision(0, source_idx, true));
                    output_collisions[output_idx] = collisions.empty() ? Polygons{} : union_(collisions);
                }
            });
    }

    // The volume cache is indexed by the topology grid. Independent output layers
    // must clip against the belt at their own physical Z, not at that cache index.
    std::vector<Polygons> output_belt_floors;
    if (layer_plan.independent() && !volumes.m_belt_floor.empty()) {
        BeltFloorContext belt_floor;
        if (belt_floor.init_local(slicing_params, print_object.print()->config(), print_object.belt_global_z_offset()))
            output_belt_floors = belt_floor.compute_per_layer_floors(layer_plan.size(),
                [&layer_plan](size_t idx) { return layer_plan[idx].print_z; });
    }
    const std::vector<Polygons> &belt_floors = output_belt_floors.empty() ? volumes.m_belt_floor : output_belt_floors;

    MeshSlicingParams mesh_slicing_params;
    mesh_slicing_params.mode = MeshSlicingParams::SlicingMode::Positive;
    trace_organic_timing("draw.rasterization.begin");
    const auto profile_planned = std::chrono::steady_clock::now();


    struct BranchResult {
        LayerIndex layer_begin{-1};
        std::vector<Polygons> slices;
        std::vector<Polygons> bottom_contacts;
    };
    const bool legacy_scheduling = std::getenv("INLONG_SUPPORT_LEGACY_SCHEDULING") != nullptr;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, trees.size(), 1),
        [&](const tbb::blocked_range<size_t> &range) {
            for (size_t tree_id = range.begin(); tree_id < range.end(); ++tree_id) {
                Tree &tree = trees[tree_id];
                LayerIndex tree_tip_layer = -1;
                for (const Branch &branch : tree.branches)
                    if (branch.has_tip)
                        tree_tip_layer = std::max(tree_tip_layer, branch.path.back()->state.layer_idx);
                const LayerIndex tree_output_end = tree_tip_layer < 0 ? LayerIndex(layer_plan.size()) :
                    (layer_plan.independent() ? layer_plan.at_or_below(layer_z(slicing_params, config, tree_tip_layer)) : tree_tip_layer) + 1;
                for (size_t batch_begin = 0; batch_begin < tree.branches.size();) {
                    // Reserve a conservative per-branch allowance across every
                    // concurrently rasterized tree. Low/unknown availability
                    // uses the original streaming order with one branch.
                    constexpr size_t branch_allowance = size_t(8) << 20;
                    const size_t desired = std::min<size_t>(64, tree.branches.size() - batch_begin);
                    SupportBatchBudget budget(legacy_scheduling ? 0 : desired * branch_allowance, support_memory_snapshot());
                    const size_t batch_size = std::min(desired, std::max<size_t>(1, budget.bytes() / branch_allowance));
                    std::vector<BranchResult> results(batch_size);
                    auto rasterize = [&](const tbb::blocked_range<size_t> &branches) {
                        indexed_triangle_set partial_mesh;
                        std::vector<float> slice_z;
                        std::vector<Polygons> bottom_contacts;
                        for (size_t branch_id = branches.begin(); branch_id < branches.end(); ++branch_id) {
                            const Branch &branch = tree.branches[batch_begin + branch_id];
                            BranchResult &result = results[branch_id];
                            // Triangulate the tube.
                            partial_mesh.clear();
                            std::pair<float, float> zspan = extrude_branch(branch.path, config, slicing_params, move_bounds, branch.has_root, partial_mesh);
                            LayerIndex layer_begin;
                            LayerIndex layer_end;
                            if (layer_plan.independent()) {
                                layer_begin = layer_plan.at_or_above_slice_z(zspan.first);
                                layer_end   = layer_plan.above_slice_z(zspan.second);
                                // Spherical caps join internal branch segments.
                                // At a true root/tip, use the same termination
                                // as the synchronized planner; slicing the cap
                                // beyond that node creates unrelated contacts
                                // below a root and body material above its roof.
                                if (branch.has_root) {
                                    const coordf_t root_z = layer_z(slicing_params, config, branch.path.front()->state.layer_idx);
                                    const LayerIndex root_idx = layer_plan.find_exact(root_z);
                                    if (root_idx >= 0)
                                        layer_begin = root_idx;
                                }
                                if (branch.has_tip) {
                                    const coordf_t tip_z = layer_z(slicing_params, config, branch.path.back()->state.layer_idx);
                                    layer_end = layer_plan.at_or_below(tip_z) + 1;
                                }
                                layer_begin = std::clamp<LayerIndex>(layer_begin, 0, LayerIndex(layer_plan.size()));
                                layer_end   = std::clamp<LayerIndex>(layer_end, layer_begin, LayerIndex(layer_plan.size()));
                            } else {
                                layer_begin = branch.has_root ?
                                    branch.path.front()->state.layer_idx :
                                    std::min(branch.path.front()->state.layer_idx, layer_idx_ceil(slicing_params, config, zspan.first));
                                layer_end = (branch.has_tip ?
                                    branch.path.back()->state.layer_idx :
                                    std::max(branch.path.back()->state.layer_idx, layer_idx_floor(slicing_params, config, zspan.second))) + 1;
                            }
                            // Junction caps may extend beyond an individual
                            // segment, but never above the last real tree tip.
                            layer_end = std::min(layer_end, tree_output_end);
                            if (layer_begin >= layer_end)
                                continue;
                            slice_z.clear();
                            for (LayerIndex layer_idx = layer_begin; layer_idx < layer_end; ++ layer_idx) {
                                const double print_z  = layer_plan.independent() ? layer_plan[size_t(layer_idx)].print_z : layer_z(slicing_params, config, layer_idx);
                                const double bottom_z = layer_plan.independent() ? layer_plan[size_t(layer_idx)].bottom_z :
                                    (layer_idx > 0 ? layer_z(slicing_params, config, layer_idx - 1) : 0.);
                                slice_z.emplace_back(float(0.5 * (bottom_z + print_z)));
                            }

                            std::vector<Polygons> slices = slice_mesh(partial_mesh, slice_z, mesh_slicing_params, throw_on_cancel);

                            // INLONG: guard against empty slices from meshing.
                            if (slices.empty())
                                continue;

                            bottom_contacts.clear();
                            // INLONG: trim tiny fragments to reduce degenerate polygon booleans.
                            const double tiny_area = tiny_area_threshold();
                            //FIXME parallelize?
                            for (LayerIndex i = 0; i < LayerIndex(slices.size()); ++i) {
                                // INLONG: safety offset when trimming collision/bed to improve robustness.
                                const Polygons &collision = layer_plan.independent() ?
                                    output_collisions[size_t(layer_begin + i)] : volumes.getCollision(0, layer_begin + i, true);
                                slices[i] = diff_clipped(slices[i], collision, ApplySafetyOffset::Yes); // FIXME parent_uses_min || draw_area.element->state.use_min_xy_dist);
                                slices[i] = intersection(slices[i], volumes.m_bed_area, ApplySafetyOffset::Yes);
                                // Belt floor: clip branch slices against the belt surface plane.
                                LayerIndex belt_idx = layer_begin + i;
                                if (belt_idx < LayerIndex(belt_floors.size()) && !belt_floors[belt_idx].empty())
                                    slices[i] = diff(slices[i], belt_floors[belt_idx]);
                                remove_small(slices[i], tiny_area);
                            }

                            size_t num_empty = 0;

                            if (slices.front().empty()) {
                                // Some of the initial layers are empty.
                                num_empty = std::find_if(slices.begin(), slices.end(), [](auto &s) { return !s.empty(); }) - slices.begin();
                            }

                            // INLONG: trim leading empty slices to keep layer indices aligned.
                            if (num_empty >= slices.size())
                                continue;

                            if (num_empty > 0) {
                                slices.erase(slices.begin(), slices.begin() + num_empty);
                                layer_begin += LayerIndex(num_empty);
                            }

                            // INLONG: use the trimmed front slice as the contact reference.
                            Polygons slice_front_contact = slices.front();

                            if (branch.has_root) {
                                if (branch.path.front()->state.to_model_gracious) {
                                    if (config.settings.support_floor_layers > 0) {
                                        // If bottom Z gap is non-zero, keep bottom contacts even when not touching the model.
                                        Polygons contacts;

                                        // INLONG: non-zero bottom Z should not be clipped by placeable areas.
                                        if (config.support_rests_on_model && config.z_distance_bottom_layers > 0 && layer_begin > 0)
                                            contacts = slice_front_contact;
                                        else {
                                            const LayerIndex source_layer = layer_plan.independent() ?
                                                layer_plan[size_t(layer_begin)].source_layer_end : layer_begin;
                                            Polygons placeable = volumes.getPlaceableAreas(0, source_layer, [] {});
                                            contacts = intersection_clipped(slice_front_contact, placeable, ApplySafetyOffset::Yes);
                                        }

                                        remove_small(contacts, tiny_area);

                                        // INLONG: ensure bottom contacts exist if clipping removed them.
                                        if (contacts.empty() && config.support_rests_on_model && layer_begin > 0 && !slice_front_contact.empty())
                                            contacts = slice_front_contact;
                                        if (!contacts.empty())
                                            bottom_contacts.emplace_back(std::move(contacts));
                                    }
                                } else if (layer_begin > 0 && (belt_floors.empty() || num_empty == 0)) {
                                    // Belt-floor clipping makes initial slices empty often; without this
                                    // gate, "verylost" branches propagate rest_support down to layer 0 and
                                    // OOM on tall belt prints.
                                    // Drop down areas that do rest non - gracefully on the model to ensure the branch actually rests on something.
                                    struct BottomExtraSlice {
                                        Polygons polygons;
                                        double   area;
                                    };
                                    std::vector<BottomExtraSlice>   bottom_extra_slices;
                                    Polygons                        rest_support;
                                    coord_t                         bottom_radius = support_element_radius(config, *branch.path.front());
                                    // Belt printer (GeneratorOnly belt floor): the tilted belt surface is the
                                    // build surface, so a branch should terminate ON the belt with a thin tip,
                                    // not stamp its full footprint straight down to Z=0 and weld neighbouring
                                    // branches into a solid floor slab. m_belt_floor is only populated in that
                                    // mode, so it doubles as the gate (no effect on other printer types).
                                    const bool                      belt_mode = !belt_floors.empty();
                                    // Don't propagate further than 1.5 * bottom radius.
                                    //LayerIndex                      layers_propagate_max = 2 * bottom_radius / config.layer_height;
                                    LayerIndex                      layers_propagate_max = 5 * bottom_radius / config.layer_height;
                                    LayerIndex                      layer_bottommost = 0;
                                    if (!branch.path.front()->state.verylost || belt_mode) {
                                        if (layer_plan.independent()) {
                                            const coordf_t max_drop = unscaled<coordf_t>(5 * bottom_radius);
                                            layer_bottommost = layer_begin;
                                            while (layer_bottommost > 0 &&
                                                   layer_plan[size_t(layer_begin)].print_z - layer_plan[size_t(layer_bottommost - 1)].print_z <= max_drop + EPSILON)
                                                --layer_bottommost;
                                        } else {
                                            layer_bottommost = std::max(0, layer_begin - layers_propagate_max);
                                        }
                                    }
                                    double                          support_area_min_radius = M_PI * sqr(double(config.branch_radius));
                                    double                          support_area_stop = std::max(0.2 * M_PI * sqr(double(bottom_radius)), 0.5 * support_area_min_radius);
                                     // Only propagate until the rest area is smaller than this threshold.
                                    //double                          support_area_min = 0.1 * support_area_min_radius;
                                    for (LayerIndex layer_idx = layer_begin - 1; layer_idx >= layer_bottommost; -- layer_idx) {
                                        LayerIndex collision_layer = (layer_idx == layer_begin - 1) ? layer_begin : layer_idx;
                                        Polygons collision;
                                        if (layer_plan.independent()) {
                                            const OrganicSupportLayerPlanEntry &entry = layer_plan[size_t(collision_layer)];
                                            for (LayerIndex source_idx = entry.source_layer_begin; source_idx <= entry.source_layer_end; ++source_idx)
                                                append(collision, volumes.getCollision(0, source_idx, false));
                                            if (!collision.empty())
                                                collision = union_(collision);
                                        } else {
                                            collision = volumes.getCollision(0, collision_layer, false);
                                        }
                                        rest_support = diff_clipped(rest_support.empty() ? slice_front_contact : rest_support, collision, ApplySafetyOffset::Yes);
                                        // Belt floor: clip propagated support at belt surface.
                                        bool belt_cut = false;
                                        if (layer_idx < LayerIndex(belt_floors.size()) && !belt_floors[layer_idx].empty()) {
                                            double area_before = area(rest_support);
                                            rest_support = diff(rest_support, belt_floors[layer_idx]);
                                            // The belt counts as "reached" only when it actually removes part
                                            // of this branch's footprint. The belt half-plane is non-empty at
                                            // every near-belt layer, so testing non-emptiness alone would
                                            // terminate a laterally-distant branch ~1 layer above true contact,
                                            // leaving a gap. Require a real area reduction instead.
                                            belt_cut = belt_mode && area(rest_support) < area_before - tiny_area;
                                        }
                                        remove_small(rest_support, tiny_area);
                                        double rest_support_area = area(rest_support);
                                        if (rest_support_area < support_area_stop)
                                            // Don't propagate a fraction of the tree contact surface.
                                            break;
                                        bottom_extra_slices.push_back({ rest_support, rest_support_area });
                                        // Belt mode: once the belt surface actually starts cutting this branch
                                        // it has reached the belt — keep this last (belt-clipped) slice as the
                                        // contact and stop, rather than stamping the footprint further down.
                                        if (belt_cut)
                                            break;
                                    }
                                    // Now remove those bottom slices that are not supported at all.
        #if 0
                                    while (! bottom_extra_slices.empty()) {
                                        const LayerIndex output_layer = layer_begin - LayerIndex(bottom_extra_slices.size());
                                        const LayerIndex source_layer = layer_plan.independent() ?
                                            layer_plan[size_t(output_layer)].source_layer_end : output_layer;
                                        Polygons this_bottom_contacts = intersection_clipped(
                                            bottom_extra_slices.back().polygons, volumes.getPlaceableAreas(0, source_layer, [] {}));
                                        if (area(this_bottom_contacts) < support_area_min)
                                            bottom_extra_slices.pop_back();
                                        else {
                                            // At least a fraction of the tree bottom is considered to be supported.
                                            if (config.settings.support_floor_layers > 0)
                                                // Turn this fraction of the tree bottom into a contact layer.
                                                bottom_contacts.emplace_back(std::move(this_bottom_contacts));
                                            break;
                                        }
                                    }
        #endif
                                    // Belt mode: no solid support-floor pad under these branches — it is what
                                    // welds neighbouring belt-terminating branches into the dense Z=0 slab.
                                    // They simply taper out onto the tilted belt as distributed thin contacts.
                                    if (!belt_mode && config.settings.support_floor_layers > 0) {
                                        Polygons contacts;
                                        if (!bottom_extra_slices.empty()) {
                                            const int contact_idx = int(bottom_extra_slices.size()) - 1; // Use the lowest contact slice as the footprint.

                                            // INLONG: non-zero bottom Z should not be clipped by placeable areas.
                                            if (config.support_rests_on_model && config.z_distance_bottom_layers > 0 && layer_begin > 0)
                                                contacts = intersection_clipped(bottom_extra_slices[contact_idx].polygons, Polygons{volumes.m_bed_area}, ApplySafetyOffset::Yes);
                                            else {
                                                const LayerIndex source_layer = layer_plan.independent() ?
                                                    layer_plan[size_t(layer_begin)].source_layer_end : layer_begin;
                                                Polygons placeable = volumes.getPlaceableAreas(0, source_layer, [] {});
                                                contacts = intersection_clipped(bottom_extra_slices[contact_idx].polygons, placeable, ApplySafetyOffset::Yes);
                                            }
                                        } else {
                                            // Fallback: use the current contact slice when no propagation happened.
                                            if (config.support_rests_on_model && config.z_distance_bottom_layers > 0 && layer_begin > 0)
                                                contacts = slice_front_contact;
                                            else {
                                                const LayerIndex source_layer = layer_plan.independent() ?
                                                    layer_plan[size_t(layer_begin)].source_layer_end : layer_begin;
                                                Polygons placeable = volumes.getPlaceableAreas(0, source_layer, [] {});
                                                contacts = intersection_clipped(slice_front_contact, placeable, ApplySafetyOffset::Yes);
                                            }
                                        }

                                        remove_small(contacts, tiny_area);

                                        if (!contacts.empty())
                                            bottom_contacts.emplace_back(std::move(contacts));

                                        // INLONG: ensure bottom contacts exist if clipping removed them.
                                        if (bottom_contacts.empty() && config.support_rests_on_model && layer_begin > 0 && !slice_front_contact.empty())
                                            bottom_contacts.emplace_back(slice_front_contact);
                                    }
                                    layer_begin -= LayerIndex(bottom_extra_slices.size());
                                    slices.insert(slices.begin(), bottom_extra_slices.size(), {});
                                    auto it_dst = slices.begin();
                                    for (auto it_src = bottom_extra_slices.rbegin(); it_src != bottom_extra_slices.rend(); ++ it_src)
                                        *it_dst ++ = std::move(it_src->polygons);
                                }

                                // INLONG: retain bottom contacts even when no placeable areas intersect.
                                // Skipped in belt mode (m_belt_floor populated) so we don't re-introduce a
                                // solid floor pad for branches that terminate on the tilted belt surface.
                                if (belt_floors.empty() &&
                                    branch.has_root && config.support_rests_on_model && branch.path.front()->state.layer_idx > 0 &&
                                    config.settings.support_floor_layers > 0 && config.z_distance_bottom_layers > 0 &&
                                    bottom_contacts.empty() && !slice_front_contact.empty())
                                    bottom_contacts.emplace_back(slice_front_contact);

                            }
                            // INLONG: bottom contacts provide the footprint; interface layers are built later.

                            result.layer_begin = layer_begin;
                            result.slices = std::move(slices);
                            result.bottom_contacts = std::move(bottom_contacts);
                        }
                    };
                    if (batch_size == 1)
                        rasterize(tbb::blocked_range<size_t>(0, 1));
                    else
                        tbb::parallel_for(tbb::blocked_range<size_t>(0, batch_size, 4), rasterize);
                    // Publication remains in the exact source branch order,
                    // including roof recovery and the order of polygon unions.
                    for (size_t branch_id = 0; branch_id < batch_size; ++branch_id) {
                        BranchResult &result = results[branch_id];
                        if (result.layer_begin < 0)
                            continue;
                        const Branch &branch = tree.branches[batch_begin + branch_id];
                        LayerIndex layer_begin = result.layer_begin;
                        LayerIndex layer_end = 0;
                        std::vector<Polygons> &slices = result.slices;
                        std::vector<Polygons> &bottom_contacts = result.bottom_contacts;
                        recover_pending_branch_roofs(interface_placer, branch.path, layer_begin, layer_plan, slices);

                        while (! slices.empty() && slices.back().empty()) {
                            slices.pop_back();
                        }

                        // INLONG: recompute layer_end after trimming trailing empty slices.
                        layer_end = layer_begin + LayerIndex(slices.size());

                        if (layer_begin < layer_end) {
                            LayerIndex new_begin = tree.first_layer_id == -1 ? layer_begin : std::min(tree.first_layer_id, layer_begin);
                            LayerIndex new_end   = tree.first_layer_id == -1 ? layer_end : std::max(tree.first_layer_id + LayerIndex(tree.slices.size()), layer_end);
                            size_t     new_size  = size_t(new_end - new_begin);
                            if (tree.first_layer_id == -1) {
                            } else if (tree.slices.capacity() < new_size) {
                                std::vector<Slice> new_slices;
                                new_slices.reserve(new_size);
                                if (LayerIndex dif = tree.first_layer_id - new_begin; dif > 0)
                                    new_slices.insert(new_slices.end(), dif, {});
                                append(new_slices, std::move(tree.slices));
                                tree.slices.swap(new_slices);
                            } else if (LayerIndex dif = tree.first_layer_id - new_begin; dif > 0)
                                tree.slices.insert(tree.slices.begin(), tree.first_layer_id - new_begin, {});
                            tree.slices.insert(tree.slices.end(), new_size - tree.slices.size(), {});
                            for (LayerIndex i = layer_begin; i != layer_end; ++ i) {
                                int j = i - layer_begin;
                                Polygons &src = slices[j];
                                bool has_bottom_contacts = j < int(bottom_contacts.size()) && !bottom_contacts[j].empty();

                                // INLONG: preserve bottom contacts even if base polygons are empty.
                                if (!src.empty() || has_bottom_contacts) {
                                    Slice &dst = tree.slices[i - new_begin];
                                    if (++ dst.num_branches > 1) {
                                        if (!src.empty())
                                            append_support_polygons(dst.polygons, std::move(src));
                                        if (has_bottom_contacts)
                                            append_support_polygons(dst.bottom_contacts, std::move(bottom_contacts[j]));
                                    } else {
                                        if (!src.empty())
                                            dst.polygons = std::move(src);
                                        if (has_bottom_contacts)
                                            dst.bottom_contacts = std::move(bottom_contacts[j]);
                                    }
                                }
                            }

                            tree.first_layer_id = new_begin;
                        }
                    }
                    batch_begin += batch_size;
                    throw_on_cancel();
                }
            }
        }, tbb::simple_partitioner());

    const auto profile_rasterized = std::chrono::steady_clock::now();
    trace_organic_timing("draw.rasterization.end");
    BOOST_LOG_TRIVIAL(info) << "Organic draw profiling: rasterization " << std::chrono::duration<double>(profile_rasterized - profile_planned).count() << " seconds";

    // The final layer assembly already performs the union needed before
    // smoothing. Defer per-tree unions to that single assembly point so the
    // same polygons are never merged once per tree and again per output layer.
    trace_organic_timing("draw.unions.begin");
    const bool defer_tree_unions = true;
    // A large tree may hold almost every branch. Schedule its layer unions
    // separately: each layer owns its output and keeps the original input order.
    size_t union_count = 0;
    size_t largest_input = 0;
    for (const Tree &tree : trees)
        for (const Slice &slice : tree.slices)
            if (!defer_tree_unions && slice.num_branches > 1) {
                ++union_count;
                size_t vertices = 0;
                for (const Polygon &polygon : slice.polygons)
                    vertices += polygon.size();
                for (const Polygon &polygon : slice.bottom_contacts)
                    vertices += polygon.size();
                largest_input = std::max(largest_input, vertices);
            }
    const size_t estimated_worker_bytes = std::max<size_t>(size_t(1) << 20, largest_input * 256);
    const size_t requested_workers = std::max(1u, std::thread::hardware_concurrency());
    const size_t job_bytes = union_count * sizeof(Slice *);
    const SupportMemorySnapshot memory = support_memory_snapshot();
    SupportBatchBudget union_budget(job_bytes + requested_workers * estimated_worker_bytes, memory);
    const bool legacy_unions = std::getenv("INLONG_SUPPORT_LEGACY_SCHEDULING") != nullptr;
    const size_t union_workers = union_count == 0 || legacy_unions || union_budget.bytes() <= job_bytes ? 1 :
        std::max<size_t>(1, (union_budget.bytes() - job_bytes) / estimated_worker_bytes);
    auto merge_slice = [&throw_on_cancel, defer_tree_unions](Slice &slice) {
        if (slice.num_branches > 1 && !defer_tree_unions) {
            if (!slice.polygons.empty())
                slice.polygons = union_(slice.polygons);
            if (!slice.bottom_contacts.empty())
                slice.bottom_contacts = union_(slice.bottom_contacts);
            slice.num_branches = 1;
        }
        throw_on_cancel();
    };
    if (union_workers <= 1) {
        // No extra job list or concurrent union temporaries under pressure.
        for (Tree &tree : trees)
            for (Slice &slice : tree.slices)
                merge_slice(slice);
    } else {
        std::vector<Slice *> jobs;
        jobs.reserve(union_count);
        for (Tree &tree : trees)
            for (Slice &slice : tree.slices)
                if (!defer_tree_unions && slice.num_branches > 1)
                    jobs.push_back(&slice);
        tbb::task_arena arena{int(std::min(requested_workers, union_workers))};
        arena.execute([&] {
            tbb::parallel_for(tbb::blocked_range<size_t>(0, jobs.size(), 1), [&](const tbb::blocked_range<size_t> &range) {
                for (size_t idx = range.begin(); idx < range.end(); ++idx)
                    merge_slice(*jobs[idx]);
            }, tbb::simple_partitioner());
        });
    }
    const auto profile_united = std::chrono::steady_clock::now();
    trace_organic_timing("draw.unions.end");
    BOOST_LOG_TRIVIAL(info) << "Organic draw profiling: tree-layer unions " << std::chrono::duration<double>(profile_united - profile_rasterized).count()
                            << " seconds, jobs " << union_count << ", workers " << union_workers << ", reserved bytes " << union_budget.bytes();

    size_t num_layers = 0;
    for (Tree &tree : trees)
        if (tree.first_layer_id >= 0)
            num_layers = std::max(num_layers, size_t(tree.first_layer_id + tree.slices.size()));

    // Model layer bounds are immutable during organic support assembly.  The
    // bottom-contact lookup still performs the authoritative Clipper
    // intersection, but can reject layers whose XY bounds cannot overlap the
    // contact footprint without entering Clipper at all.
    const auto &model_layers = print_object.layers();
    std::vector<BoundingBox> model_layer_bounds;
    model_layer_bounds.reserve(model_layers.size());
    for (const Layer *layer : model_layers)
        model_layer_bounds.push_back(get_extents(layer->lslices));

    std::vector<Slice> slices(num_layers, Slice{});
    for (Tree &tree : trees)
        if (tree.first_layer_id >= 0) {
            for (LayerIndex i = tree.first_layer_id; i != tree.first_layer_id + LayerIndex(tree.slices.size()); ++ i) {
                Slice &src = tree.slices[i - tree.first_layer_id];
                bool has_bottom_contacts = !src.bottom_contacts.empty();

                // INLONG: preserve bottom contacts even if base polygons are empty.
                if (!src.polygons.empty() || has_bottom_contacts) {
                    Slice &dst = slices[i];

                    const size_t source_branches = src.num_branches;
                    const bool had_branches = dst.num_branches != 0;
                    dst.num_branches += source_branches;
                    if (had_branches) {
                        if (!src.polygons.empty())
                            append_support_polygons(dst.polygons, std::move(src.polygons));
                        if (has_bottom_contacts)
                            append_support_polygons(dst.bottom_contacts, std::move(src.bottom_contacts));
                    } else {
                        if (!src.polygons.empty())
                            dst.polygons = std::move(src.polygons);
                        if (has_bottom_contacts)
                            dst.bottom_contacts = std::move(src.bottom_contacts);
                    }
                }
            }
        }

    trace_organic_timing("draw.final_assembly.begin");
    tbb::parallel_for(tbb::blocked_range<size_t>(0, slices.size(), 1),
        [&print_object, &config, &slices, &bottom_contacts, &top_contacts, &intermediate_layers, &layer_storage, &layer_plan, &model_layer_bounds, &throw_on_cancel](const tbb::blocked_range<size_t> &range) {
        for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
            Slice &slice = slices[layer_idx];
            assert(intermediate_layers[layer_idx] == nullptr);
            // INLONG: avoid union_ on empty inputs.
            Polygons base_layer_polygons;
            if (slice.polygons.size() == 1 && slice.polygons.front().is_valid())
                base_layer_polygons = std::move(slice.polygons);
            else if (!slice.polygons.empty())
                base_layer_polygons = union_(slice.polygons);
            Polygons bottom_contact_polygons;
            if (slice.bottom_contacts.size() == 1 && slice.bottom_contacts.front().is_valid())
                bottom_contact_polygons = std::move(slice.bottom_contacts);
            else if (!slice.bottom_contacts.empty())
                bottom_contact_polygons = union_(slice.bottom_contacts);

            if (! base_layer_polygons.empty()) {
                // Most of the time in this function is this union call. Can take 300+ ms when a lot of areas are to be unioned.
                base_layer_polygons = organic_smooth_outward_if_needed(
                    std::move(base_layer_polygons),
                    config.support_line_width); //FIXME was .smooth(50);
                //smooth_outward(closing(std::move(bottom), closing_distance + minimum_island_radius, closing_distance, SUPPORT_SURFACES_OFFSET_PARAMETERS), smoothing_distance) :
                // simplify a bit, to ensure the output does not contain outrageous amounts of vertices. Should not be necessary, just a precaution.
                base_layer_polygons = polygons_simplify(base_layer_polygons, std::min(scaled<double>(0.03), double(config.resolution)));
            }

            // Subtract top contact layer polygons from support base.
            SupportGeneratorLayer *top_contact_layer = top_contacts.empty() ? nullptr : top_contacts[layer_idx];
            if (top_contact_layer && ! top_contact_layer->polygons.empty() && ! base_layer_polygons.empty()) {
                base_layer_polygons = diff(base_layer_polygons, top_contact_layer->polygons);
                if (! bottom_contact_polygons.empty())
                    //FIXME it may be better to clip bottom contacts with top contacts first after they are propagated to produce interface layers.
                    bottom_contact_polygons = diff(bottom_contact_polygons, top_contact_layer->polygons);
            }
            if (! bottom_contact_polygons.empty()) {
                // A gracious root can partly rest on another tree instead of
                // the model. Only its actual model projection is a bottom
                // interface; leave the rest in the ordinary branch polygons.
                Polygons model_contacts;
                const coordf_t gap = print_object.slicing_parameters().gap_object_support;
                const coordf_t bottom_z = layer_plan.independent() ? layer_plan[layer_idx].bottom_z :
                    (layer_idx > 0 ? layer_z(print_object.slicing_parameters(), config, layer_idx - 1) : 0.);
                ExPolygons contact_components;
                if (bottom_contact_polygons.size() == 1 && bottom_contact_polygons.front().is_valid())
                    contact_components.emplace_back(std::move(bottom_contact_polygons.front()));
                else
                    contact_components = union_ex(bottom_contact_polygons);
                for (const ExPolygon &component : contact_components) {
                    const Polygons footprint = to_polygons(component);
                    if (const Layer *model = organic_bottom_contact_model_layer(print_object, footprint, bottom_z - gap, &model_layer_bounds))
                        append(model_contacts, intersection(model->lslices, footprint));
                }
                if (model_contacts.size() == 1 && model_contacts.front().is_valid())
                    bottom_contact_polygons = std::move(model_contacts);
                else
                    bottom_contact_polygons = model_contacts.empty() ? Polygons{} : union_(model_contacts);
            }
            if (! bottom_contact_polygons.empty()) {
                base_layer_polygons = diff(base_layer_polygons, bottom_contact_polygons);
                SupportGeneratorLayer *bottom_contact_layer;
                if (layer_plan.independent()) {
                    bottom_contact_layer = &layer_storage.allocate(SupporLayerType::BottomContact);
                    layer_plan.initialize(*bottom_contact_layer, layer_idx);
                } else {
                    bottom_contact_layer = &layer_allocate(
                        layer_storage, SupporLayerType::BottomContact, print_object.slicing_parameters(), config, layer_idx);
                }
                bottom_contacts[layer_idx] = bottom_contact_layer;
                bottom_contact_layer->polygons = std::move(bottom_contact_polygons);
            }
            if (! base_layer_polygons.empty()) {
                SupportGeneratorLayer *base_layer;
                if (layer_plan.independent()) {
                    base_layer = &layer_storage.allocate(SupporLayerType::Base);
                    layer_plan.initialize(*base_layer, layer_idx);
                } else {
                    base_layer = &layer_allocate(
                        layer_storage, SupporLayerType::Base, print_object.slicing_parameters(), config, layer_idx);
                }
                intermediate_layers[layer_idx] = base_layer;
                base_layer->polygons = std::move(base_layer_polygons);
            }

            throw_on_cancel();
        }
    }, tbb::simple_partitioner());

    trace_organic_timing("draw.final_assembly.end");
    trace_organic_timing("draw.anchoring.begin");
    const bool repaired = anchor_organic_support_components(print_object, volumes, config, layer_plan, support_params,
        bottom_contacts, top_contacts, interface_placer.top_interfaces_mutable(),
        interface_placer.top_base_interfaces_mutable(), intermediate_layers, layer_storage, throw_on_cancel);
    trace_organic_timing("draw.anchoring.end");
    if (source_repaired != nullptr)
        *source_repaired = repaired;
    if (repaired)
        BOOST_LOG_TRIVIAL(info) << "Organic source repair changed support topology";

    BOOST_LOG_TRIVIAL(info) << "Organic draw profiling: final polygon assembly " << std::chrono::duration<double>(std::chrono::steady_clock::now() - profile_united).count() << " seconds";
    return layer_plan;
}

// Independent organic layers use a variable Z grid for the branch mesh, but a
// bottom contact still has to be measured from its actual model surface.
// Split a footprint at visible model steps and retain a legal contact/branch
// handoff. Moving only the contact leaves the old root suspended above it.
static void normalize_organic_bottom_contacts(
    const PrintObject &object, TreeModelVolumes &volumes, const TreeSupportSettings &config,
    const SupportParameters &support_params,
    const OrganicSupportLayerPlan &plan,
    SupportGeneratorLayersPtr &bottom_contacts,
    SupportGeneratorLayersPtr &intermediate_layers,
    SupportGeneratorLayerStorage &storage)
{
    const auto &parameters = object.slicing_parameters();
    const coordf_t gap = parameters.gap_object_support;
    if (gap <= EPSILON || bottom_contacts.empty())
        return;
    const coordf_t minimum_height = parameters.min_layer_height;
    const coordf_t maximum_height = parameters.max_suport_layer_height;
    const double minimum_area = sqr(double(scale_(0.001)));
    SupportGeneratorLayersPtr normalized;
    struct RootContinuation {
        SupportGeneratorLayer *contact;
        coordf_t source_top;
        Polygons footprint;
    };
    std::vector<RootContinuation> continuations;
    auto append_root = [&](const SupportGeneratorLayer &source, const Layer &model, Polygons footprint) {
        const coordf_t bottom = model.print_z + gap;
        const coordf_t preferred = bottom + source.height;
        coordf_t top = preferred;
        double best_shift = std::abs(preferred - source.print_z) <= EPSILON ? 0. :
            std::numeric_limits<double>::max();
        // Two local extrusion heights must be legal. A nearly coincident
        // global plane can belong to an unrelated tree and cannot be used to
        // introduce a sub-minimum sliver above this root.
        for (size_t idx = 0; best_shift > EPSILON && idx < plan.size() && plan[idx].print_z <= source.print_z + EPSILON; ++idx) {
            const coordf_t low = std::max(bottom + minimum_height, plan[idx].print_z - maximum_height);
            const coordf_t high = std::min(bottom + maximum_height, plan[idx].print_z - minimum_height);
            if (low > high + EPSILON)
                continue;
            const coordf_t candidate = std::clamp(preferred, low, std::max(low, high));
            const double shift = std::abs(candidate - preferred);
            if (shift < best_shift) {
                top = candidate;
                best_shift = shift;
            }
        }
        if (!std::isfinite(best_shift) || best_shift == std::numeric_limits<double>::max()) {
            // A short original root may fit one contact layer but not two
            // minimum-height extrusions. Keep that existing legal top plane.
            if (source.print_z - bottom >= minimum_height - EPSILON &&
                source.print_z - bottom <= maximum_height + EPSILON)
                top = source.print_z;
        }
        auto &contact = storage.allocate(SupporLayerType::BottomContact);
        contact.print_z = top;
        contact.bottom_z = model.print_z;
        contact.height = top - bottom;
        contact.idx_object_layer_below = model.id();
        contact.polygons = std::move(footprint);
        normalized.push_back(&contact);
        if (source.print_z > top + EPSILON)
            continuations.push_back({&contact, source.print_z, contact.polygons});
        return &contact;
    };
    for (SupportGeneratorLayer *contact : bottom_contacts) {
        if (contact == nullptr || contact->polygons.empty() || contact->height <= EPSILON)
            continue;
        const coordf_t expected_model_z = contact->bottom_print_z() - gap;
        const Polygons source_footprint = contact->polygons;
        std::vector<SupportGeneratorLayer *> roots;
        Polygons remaining = contact->polygons;
        const auto &models = object.layers();
        auto model = std::upper_bound(models.begin(), models.end(), expected_model_z + EPSILON,
            [](double z, const Layer *layer) { return z < layer->print_z; });
        while (model != models.begin() && !remaining.empty()) {
            const Layer &surface = **--model;
            Polygons footprint = intersection(remaining, to_polygons(surface.lslices));
            if (area(footprint) <= minimum_area)
                continue;
            roots.push_back(append_root(*contact, surface, footprint));
            remaining = diff(remaining, footprint);
        }
        if (support_params.bottom_contact_wall_count > 0 && roots.size() > 1) {
            const ExPolygons components = union_ex(source_footprint);
            for (SupportGeneratorLayer *root : roots) {
                Polygons wall_region = root->polygons;
                for (const ExPolygon &component : components) {
                    const Polygons section = to_polygons(component);
                    if (area(intersection(section, root->polygons)) <= minimum_area) continue;
                    Polygons lower_bearing;
                    for (const SupportGeneratorLayer *below : roots)
                        if (below->print_z >= root->bottom_print_z() - EPSILON &&
                            root->print_z - below->print_z >= minimum_height - EPSILON)
                            append(lower_bearing, intersection(section, below->polygons));
                    if (lower_bearing.empty()) continue;
                    // Keep the original parent topology across a stepped
                    // landing. Extra rim spans are over an existing lower
                    // contact and must pass the same physical slab/blocker
                    // check; the true model-contact patches remain unchanged.
                    append(wall_region, diff(lower_bearing, organic_support_slab_collision(
                        object, volumes, config, support_params, root->bottom_print_z(), root->print_z)));
                }
                if (area(diff(wall_region, root->polygons)) > minimum_area)
                    root->contact_polygons = std::make_unique<Polygons>(intersection(
                        union_(wall_region), volumes.m_bed_area));
            }
        }
        // Preserve a root without an identifiable lower model surface; it
        // still has to pass the existing landing and collision checks.
        if (area(remaining) > minimum_area) {
            contact->polygons = std::move(remaining);
            normalized.push_back(contact);
        } else
            contact->polygons.clear();
    }
    bottom_contacts = std::move(normalized);

    // Contacts above the same model surface have the same physical bottom.
    // Different branch rasterization intervals must not leave two contact
    // tops a few microns apart: overlapping footprints would then be emitted
    // with a sub-minimum modulated flow height. Keep the lowest legal contact
    // plane and union its bearing footprint before the interface stack is made.
    bottom_contacts.erase(std::remove_if(bottom_contacts.begin(), bottom_contacts.end(),
        [](const SupportGeneratorLayer *layer) { return layer == nullptr || layer->polygons.empty(); }), bottom_contacts.end());
    std::stable_sort(bottom_contacts.begin(), bottom_contacts.end(),
        [](const SupportGeneratorLayer *a, const SupportGeneratorLayer *b) { return a->print_z < b->print_z; });
    std::map<SupportGeneratorLayer *, SupportGeneratorLayer *> merged_contacts;
    for (size_t i = 0; i < bottom_contacts.size(); ++i) {
        SupportGeneratorLayer &first = *bottom_contacts[i];
        if (first.polygons.empty())
            continue;
        for (size_t j = i + 1; j < bottom_contacts.size(); ++j) {
            SupportGeneratorLayer &next = *bottom_contacts[j];
            if (next.print_z - first.print_z >= object.slicing_parameters().min_layer_height - EPSILON)
                break;
            if (next.polygons.empty() || next.idx_object_layer_below != first.idx_object_layer_below ||
                std::abs(next.bottom_print_z() - first.bottom_print_z()) > EPSILON)
                continue;
            if (first.contact_polygons || next.contact_polygons)
                first.contact_polygons = std::make_unique<Polygons>(union_(
                    first.contact_polygons ? *first.contact_polygons : first.polygons,
                    next.contact_polygons ? *next.contact_polygons : next.polygons));
            first.polygons = union_(first.polygons, next.polygons);
            next.polygons.clear();
            merged_contacts[&next] = &first;
        }
    }
    bottom_contacts.erase(std::remove_if(bottom_contacts.begin(), bottom_contacts.end(),
        [](const SupportGeneratorLayer *layer) { return layer->polygons.empty(); }), bottom_contacts.end());
    for (const RootContinuation &root : continuations) {
        SupportGeneratorLayer *contact = root.contact;
        for (auto merged = merged_contacts.find(contact); merged != merged_contacts.end(); merged = merged_contacts.find(contact))
            contact = merged->second;
        coordf_t predecessor = contact->print_z;
        for (size_t idx = 0; idx < plan.size() && plan[idx].print_z <= root.source_top + EPSILON; ++idx) {
            const coordf_t height = plan[idx].print_z - predecessor;
            if (height < minimum_height - EPSILON || height > maximum_height + EPSILON)
                continue;
            // A relocated contact changes the physical bottom of the first
            // branch extrusion. Source-grid collision masks reserve an extra
            // model layer here and can erase the entire valid root corridor.
            Polygons continuation = diff(root.footprint, organic_support_slab_collision(
                object, volumes, config, support_params, predecessor, plan[idx].print_z));
            if (continuation.empty())
                continue;
            if (intermediate_layers.size() <= idx)
                intermediate_layers.resize(idx + 1, nullptr);
            SupportGeneratorLayer *&base = intermediate_layers[idx];
            if (base == nullptr) {
                base = &storage.allocate(SupporLayerType::Base);
                plan.initialize(*base, idx);
            }
            base->height = std::max(base->height, height);
            base->bottom_z = base->print_z - base->height;
            base->polygons = union_(base->polygons, continuation);
            predecessor = base->print_z;
        }
    }
}

} // namespace TreeSupport3D

void generate_tree_support_3D(PrintObject &print_object, TreeSupport* tree_support, std::function<void()> throw_on_cancel)
{
    size_t idx = 0;
    for (const PrintObject *po : print_object.print()->objects()) {
        if (po == &print_object)
            break;
        ++idx;
    }

    Points bedpts = tree_support->m_machine_border.contour.points;
    Pointfs bedptsf;
    std::transform(bedpts.begin(), bedpts.end(), std::back_inserter(bedptsf), [](const Point &p) { return unscale(p); });
    BuildVolume build_volume{ bedptsf, tree_support->m_print_config->printable_height, {}, {} };

    TreeSupport3D::generate_support_areas(*print_object.print(), tree_support, build_volume, { idx }, throw_on_cancel);
}

} // namespace Slic3r

