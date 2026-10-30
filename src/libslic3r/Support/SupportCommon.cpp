#include "../ClipperUtils.hpp"
#include "../ClipperZUtils.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../Fill/FillBase.hpp"
#include "../VariableWidth.hpp"
#include "../MutablePolygon.hpp"
#include "../Geometry.hpp"
#include "../Geometry/ConvexHull.hpp"
#include "../Point.hpp"

#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <new>
#include <deque>
#include <boost/container/static_vector.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <map>
#include <numeric>
#include <set>
#include <tuple>
#include <thread>
#include <unordered_map>
#include <tbb/global_control.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>
#include <clipper2/clipper.h>

#include "SupportCommon.hpp"
#include "SupportLayer.hpp"
#include "SupportParameters.hpp"
#include "SupportCacheBudget.hpp"

// #define SLIC3R_DEBUG

// Make assert active if SLIC3R_DEBUG
#ifdef SLIC3R_DEBUG
    #define DEBUG
    #define _DEBUG
    #undef NDEBUG
    #include "../utils.hpp"
    #include "../SVG.hpp"
#endif

#include <cassert>
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/MultiMaterialSegmentation.hpp"

namespace Slic3r {

// how much we extend support around the actual contact area
//FIXME this should be dependent on the nozzle diameter!
#define SUPPORT_MATERIAL_MARGIN 1.5

//#define SUPPORT_SURFACES_OFFSET_PARAMETERS jtMiter, 3.
//#define SUPPORT_SURFACES_OFFSET_PARAMETERS jtMiter, 1.5
#define SUPPORT_SURFACES_OFFSET_PARAMETERS jtSquare, 0.

ExPolygons build_raft_first_layer_footprint(
    const ExPolygons &object_areas,
    const ExPolygons &support_areas,
    bool              generate_bounding_box,
    bool              ignore_internal_contours)
{
    if (generate_bounding_box) {
        BoundingBox bbox = get_extents(object_areas);
        if (!support_areas.empty()) {
            const BoundingBox support_bbox = get_extents(support_areas);
            if (support_bbox.defined)
                bbox.merge(support_bbox);
        }

        if (!bbox.defined)
            return {};

        ExPolygon footprint;
        footprint.contour = bbox.polygon();
        footprint.contour.make_counter_clockwise();
        return {std::move(footprint)};
    }

    ExPolygons footprint;
    footprint.reserve(object_areas.size() + support_areas.size());
    if (ignore_internal_contours) {
        for (const ExPolygon &area : object_areas) {
            ExPolygon outer_area;
            outer_area.contour = area.contour;
            outer_area.contour.make_counter_clockwise();
            footprint.emplace_back(std::move(outer_area));
        }
    } else {
        footprint.insert(footprint.end(), object_areas.begin(), object_areas.end());
    }
    footprint.insert(footprint.end(), support_areas.begin(), support_areas.end());
    return footprint;
}

coordf_t raft_layer_expansion_offset(
    const PrintObjectConfig &config,
    size_t                   raft_layer_count,
    size_t                   layer_id)
{
    if (raft_layer_count <= 1 || layer_id >= raft_layer_count)
        return 0.;
    return config.raft_layer_expansion_step.value * coordf_t(raft_layer_count - layer_id - 1);
}

namespace {

void append_valid_clipped_paths(
    const ExtrusionPath         &path,
    const ExPolygons            &occupied,
    ExtrusionEntityCollection   &out)
{
    ExtrusionEntityCollection fragments;
    path.subtract_expolygons(occupied, &fragments);
    for (ExtrusionEntity *entity : fragments.entities) {
        const auto *fragment = dynamic_cast<const ExtrusionPath *>(entity);
        if (fragment != nullptr && fragment->polyline.points.size() >= 2 && fragment->length() > EPSILON)
            out.entities.emplace_back(entity);
        else
            delete entity;
    }
    fragments.entities.clear();
}

void append_paths_without_overlap(
    const ExtrusionEntity &entity,
    const ExPolygons      &occupied,
    ExtrusionEntityCollection &out)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            append_paths_without_overlap(*child, occupied, out);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
        append_valid_clipped_paths(*path, occupied, out);
    } else if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
        for (const ExtrusionPath &path : multipath->paths)
            append_valid_clipped_paths(path, occupied, out);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
        for (const ExtrusionPath &path : loop->paths)
            append_valid_clipped_paths(path, occupied, out);
    } else {
        throw Slic3r::InvalidArgument("Unsupported raft extrusion entity");
    }
}

} // namespace

std::vector<std::unique_ptr<ExtrusionEntityCollection>> clip_overlapping_raft_paths(
    const std::vector<RaftPathInstance> &instances)
{
    std::vector<std::unique_ptr<ExtrusionEntityCollection>> result(instances.size());
    ExPolygons occupied;

    for (size_t index = 0; index < instances.size(); ++index) {
        const RaftPathInstance &instance = instances[index];
        if (instance.paths == nullptr || instance.paths->empty())
            continue;

        ExPolygons footprint = instance.footprint == nullptr ? ExPolygons{} : *instance.footprint;
        if (footprint.empty())
            footprint = union_ex(instance.paths->polygons_covered_by_spacing());
        if (footprint.empty())
            continue;

        ExPolygons global_footprint = footprint;
        translate(global_footprint, instance.shift);

        if (!occupied.empty()) {
            ExPolygons local_occupied = occupied;
            translate(local_occupied, -instance.shift);
            if (!intersection_ex(footprint, local_occupied).empty()) {
                auto clipped = std::make_unique<ExtrusionEntityCollection>();
                clipped->no_sort = instance.paths->no_sort;
                append_paths_without_overlap(*instance.paths, local_occupied, *clipped);
                result[index] = std::move(clipped);
            }
        }

        expolygons_append(occupied, std::move(global_footprint));
        occupied = union_ex(occupied);
    }

    return result;
}

// Convert some of the intermediate layers into top/bottom interface layers as well as base interface layers.
std::pair<SupportGeneratorLayersPtr, SupportGeneratorLayersPtr> generate_interface_layers(
    const PrintObjectConfig           &config,
    const SupportParameters           &support_params,
    const SupportGeneratorLayersPtr   &bottom_contacts,
    const SupportGeneratorLayersPtr   &top_contacts,
    // Input / output, will be merged with output. Only provided for Organic supports.
    SupportGeneratorLayersPtr         &top_interface_layers,
    SupportGeneratorLayersPtr         &top_base_interface_layers,
    // Input, will be trimmed with the newly created interface layers.
    SupportGeneratorLayersPtr         &intermediate_layers,
    SupportGeneratorLayerStorage      &layer_storage)
{
    std::pair<SupportGeneratorLayersPtr, SupportGeneratorLayersPtr> base_and_interface_layers;

    if (! intermediate_layers.empty() && support_params.has_interfaces()) {
        const auto profile_started = std::chrono::steady_clock::now();
        const bool legacy_interface = support_params.support_style != smsTreeOrganic ||
                                      std::getenv("INLONG_SUPPORT_LEGACY_INTERFACE") != nullptr;
        // For all intermediate layers, collect top contact surfaces, which are not further than support_material_interface_layers.
        BOOST_LOG_TRIVIAL(debug) << "PrintObjectSupportMaterial::generate_interface_layers() in parallel - start";
        const bool                 snug_supports          = support_params.support_style == smsSnug;
        const bool                 smooth_supports        = support_params.support_style != smsGrid;
        SupportGeneratorLayersPtr &interface_layers       = base_and_interface_layers.first;
        SupportGeneratorLayersPtr &base_interface_layers  = base_and_interface_layers.second;
        // Contacts printed separately consume one requested interface layer.
        // Organic bottom contacts are emitted as real toolpaths too, so they
        // must consume the same configured layer as classic support contacts.
        const size_t num_top_interface_layers         = support_params.has_top_contacts ?
                                                        support_params.num_top_interface_layers - 1 : 0;
        const size_t num_bottom_interface_layers      = support_params.has_bottom_contacts ?
                                                        support_params.num_bottom_interface_layers - 1 : 0;
        const size_t num_top_base_interface_layers    = std::min(support_params.num_top_base_interface_layers,    num_top_interface_layers);
        const size_t num_bottom_base_interface_layers = std::min(support_params.num_bottom_base_interface_layers, num_bottom_interface_layers);
        const size_t num_top_interface_layers_only    = num_top_interface_layers    - num_top_base_interface_layers;
        const size_t num_bottom_interface_layers_only = num_bottom_interface_layers - num_bottom_base_interface_layers;

        interface_layers.assign(intermediate_layers.size(), nullptr);
        if (support_params.has_base_interfaces())
            base_interface_layers.assign(intermediate_layers.size(), nullptr);
        const auto smoothing_distance    = support_params.support_material_interface_flow.scaled_spacing() * 1.5;
        // INLONG: use top/bottom interface densities for smoothing.
        const auto minimum_island_radius_top = support_params.support_material_interface_flow.scaled_spacing() / support_params.top_interface_density;
        const auto minimum_island_radius_bottom = support_params.support_material_interface_flow.scaled_spacing() / support_params.bottom_interface_density;
        const auto closing_distance      = smoothing_distance; // scaled<float>(config.support_material_closing_radius.value);
        const coord_t organic_seam_distance = std::max<coord_t>(1,
            support_params.support_material_interface_flow.scaled_width() / 2);
        // Insert a new layer into base_interface_layers, if intersection with base exists.
        // INLONG: regularize top and bottom interfaces with separate minimum island radii.
        auto insert_layer = [&layer_storage, legacy_interface, smooth_supports, closing_distance, smoothing_distance,
                             minimum_island_radius_top, minimum_island_radius_bottom, organic_seam_distance](
                SupportGeneratorLayer &intermediate_layer, Polygons &bottom, Polygons &&top, SupportGeneratorLayer *top_interface_layer,
                const Polygons *subtract, SupporLayerType type) -> SupportGeneratorLayer* {
            bool has_top_interface = top_interface_layer && ! top_interface_layer->polygons.empty();
            assert(! bottom.empty() || ! top.empty() || has_top_interface);
            // INLONG: regularize interfaces using the top/bottom radii.
            auto regularize = [&](Polygons polys, coordf_t minimum_island_radius, bool top_projection) -> Polygons {
                if (polys.empty())
                    return polys;
                // Remove intersecting contours before expanding closed paths.
                // Otherwise the same overlapping roof fragments are expanded
                // independently and only merged by the expensive offset stage.
                if (smooth_supports && !legacy_interface) {
                    polys = union_(polys);
                    // Organic roofs already carry the collision-safe contact
                    // footprint. The classic density-dependent outward smoothing
                    // converts nearby trunks into extra interface platforms.
                    // Close only sub-bead seams in the real projected demand;
                    // leave the remaining region to the structural tree planner.
                    // Keep the established bed foundation unchanged.
                    if (top_projection && intermediate_layer.bottom_z > EPSILON)
                        return to_polygons(closing_ex(polys, organic_seam_distance));
                }
                return smooth_supports ?
                    smooth_outward(closing(std::move(polys), closing_distance + minimum_island_radius, closing_distance, SUPPORT_SURFACES_OFFSET_PARAMETERS), smoothing_distance) :
                    union_safety_offset(std::move(polys));
            };
            // INLONG: apply independent smoothing to bottom vs top.
            // A model landing must retain the normal transition from its
            // contact pad into the wider trunk footprint. Bound roof demand,
            // while retaining that existing bottom-interface regularization.
            Polygons bottom_polys = regularize(std::move(bottom), minimum_island_radius_bottom, false);
            Polygons top_polys = regularize(std::move(top), minimum_island_radius_top, true);
            append(bottom_polys, std::move(top_polys));
            bottom = intersection(std::move(bottom_polys), intermediate_layer.polygons);
            if (has_top_interface) {
                // Don't trim the precomputed Organic supports top interface with base layer
                // as the precomputed top interface likely expands over multiple tree tips.
                bottom = union_(std::move(top_interface_layer->polygons), bottom);
                top_interface_layer->polygons.clear();
            }
            if (! bottom.empty()) {
                //FIXME Remove non-printable tiny islands, let them be printed using the base support.
                //bottom = opening(std::move(bottom), minimum_island_radius);
                if (! bottom.empty()) {
                    SupportGeneratorLayer &layer_new = top_interface_layer ? *top_interface_layer : layer_storage.allocate(type);
                    layer_new.polygons   = std::move(bottom);
                    layer_new.print_z    = intermediate_layer.print_z;
                    layer_new.bottom_z   = intermediate_layer.bottom_z;
                    layer_new.height     = intermediate_layer.height;
                    layer_new.bridging   = intermediate_layer.bridging;
                    // Subtract the interface from the base regions.
                    intermediate_layer.polygons = diff(intermediate_layer.polygons, layer_new.polygons);
                    if (subtract)
                        // Trim the base interface layer with the interface layer.
                        layer_new.polygons = diff(std::move(layer_new.polygons), *subtract);
                    //FIXME filter layer_new.polygons islands by a minimum area?
        //                  $interface_area = [ grep abs($_->area) >= $area_threshold, @$interface_area ];
                    return &layer_new;
                }
            }
            return nullptr;
        };
        tbb::parallel_for(tbb::blocked_range<int>(0, int(intermediate_layers.size())),
            [&bottom_contacts, &top_contacts, &top_interface_layers, &top_base_interface_layers, &intermediate_layers, &insert_layer,
             num_top_interface_layers, num_bottom_interface_layers, num_top_base_interface_layers, num_bottom_base_interface_layers,
             num_top_interface_layers_only, num_bottom_interface_layers_only,
             snug_supports, &interface_layers, &base_interface_layers](const tbb::blocked_range<int>& range) {
                // Gather the top / bottom contact layers intersecting with num_interface_layers resp. num_interface_layers_only intermediate layers above / below
                // this intermediate layer.
                // Index of the first top contact layer intersecting the current intermediate layer.
                auto idx_top_contact_first        = -1;
                // Index of the first bottom contact layer intersecting the current intermediate layer.
                auto idx_bottom_contact_first     = -1;
                // Index of the first top interface layer intersecting the current intermediate layer.
                auto idx_top_interface_first      = -1;
                // Index of the first top contact interface layer intersecting the current intermediate layer.
                auto idx_top_base_interface_first = -1;
                auto num_intermediate = int(intermediate_layers.size());
                for (int idx_intermediate_layer = range.begin(); idx_intermediate_layer < range.end(); ++ idx_intermediate_layer) {
                    SupportGeneratorLayer &intermediate_layer = *intermediate_layers[idx_intermediate_layer];
                    Polygons polygons_top_contact_projected_interface;
                    Polygons polygons_top_contact_projected_base;
                    Polygons polygons_bottom_contact_projected_interface;
                    Polygons polygons_bottom_contact_projected_base;
                    if (num_top_interface_layers > 0) {
                        // Top Z coordinate of a slab, over which we are collecting the top / bottom contact surfaces
                        coordf_t top_z              = intermediate_layers[std::min(num_intermediate - 1, idx_intermediate_layer + int(num_top_interface_layers) - 1)]->print_z;
                        coordf_t top_interface_z     = std::numeric_limits<coordf_t>::max();
                        if (num_top_base_interface_layers > 0)
                            // Some top base interface layers will be generated.
                            top_interface_z = num_top_interface_layers_only == 0 ?
                                // Only base interface layers to generate.
                                - std::numeric_limits<coordf_t>::max() :
                                intermediate_layers[std::min(num_intermediate - 1, idx_intermediate_layer + int(num_top_interface_layers_only) - 1)]->print_z;
                        // Move idx_top_contact_first up until above the current print_z.
                        idx_top_contact_first = idx_higher_or_equal(top_contacts, idx_top_contact_first, [&intermediate_layer](const SupportGeneratorLayer *layer){ return layer->print_z >= intermediate_layer.print_z; }); //  - EPSILON
                        // Collect the top contact areas above this intermediate layer, below top_z.
                        for (int idx_top_contact = idx_top_contact_first; idx_top_contact < int(top_contacts.size()); ++ idx_top_contact) {
                            const SupportGeneratorLayer &top_contact_layer = *top_contacts[idx_top_contact];
                            //FIXME maybe this adds one interface layer in excess?
                            if (top_contact_layer.bottom_z - EPSILON > top_z)
                                break;
                            polygons_append(top_contact_layer.bottom_z - EPSILON > top_interface_z ? polygons_top_contact_projected_base : polygons_top_contact_projected_interface,
                                // For snug supports, project the overhang polygons covering the whole overhang, so that they will merge without a gap with support polygons of the other layers.
                                // For grid supports, merging of support regions will be performed by the projection into grid.
                                snug_supports ? *top_contact_layer.overhang_polygons : top_contact_layer.polygons);
                        }
                    }
                    if (num_bottom_interface_layers > 0) {
                        // Bottom Z coordinate of a slab, over which we are collecting the top / bottom contact surfaces
                        coordf_t bottom_z           = intermediate_layers[std::max(0, idx_intermediate_layer - int(num_bottom_interface_layers) + 1)]->bottom_z;
                        coordf_t bottom_interface_z = - std::numeric_limits<coordf_t>::max();
                        if (num_bottom_base_interface_layers > 0)
                            // Some bottom base interface layers will be generated.
                            bottom_interface_z = num_bottom_interface_layers_only == 0 ?
                                // Only base interface layers to generate.
                                std::numeric_limits<coordf_t>::max() :
                                intermediate_layers[std::max(0, idx_intermediate_layer - int(num_bottom_interface_layers_only))]->bottom_z;
                        // Move idx_bottom_contact_first up until touching bottom_z.
                        idx_bottom_contact_first = idx_higher_or_equal(bottom_contacts, idx_bottom_contact_first, [bottom_z](const SupportGeneratorLayer *layer){ return layer->print_z >= bottom_z - EPSILON; });
                        // Collect the top contact areas above this intermediate layer, below top_z.
                        for (int idx_bottom_contact = idx_bottom_contact_first; idx_bottom_contact < int(bottom_contacts.size()); ++ idx_bottom_contact) {
                            const SupportGeneratorLayer &bottom_contact_layer = *bottom_contacts[idx_bottom_contact];
                            if (bottom_contact_layer.print_z - EPSILON > intermediate_layer.bottom_z)
                                break;
                            polygons_append(bottom_contact_layer.print_z - EPSILON > bottom_interface_z ? polygons_bottom_contact_projected_interface : polygons_bottom_contact_projected_base, bottom_contact_layer.polygons);
                        }
                    }
                    auto resolve_same_layer = [](SupportGeneratorLayersPtr &layers, int &idx, coordf_t print_z) -> SupportGeneratorLayer* {
                        if (! layers.empty()) {
                            idx = idx_higher_or_equal(layers, idx, [print_z](const SupportGeneratorLayer *layer) { return layer->print_z > print_z - EPSILON; });
                            if (idx < int(layers.size()) && layers[idx]->print_z < print_z + EPSILON)
                                return layers[idx];
                        }
                        return nullptr;
                    };
                    SupportGeneratorLayer *top_interface_layer      = resolve_same_layer(top_interface_layers, idx_top_interface_first, intermediate_layer.print_z);
                    SupportGeneratorLayer *top_base_interface_layer = resolve_same_layer(top_base_interface_layers, idx_top_base_interface_first, intermediate_layer.print_z);
                    SupportGeneratorLayer *interface_layer          = nullptr;
                    if (! polygons_bottom_contact_projected_interface.empty() || ! polygons_top_contact_projected_interface.empty() ||
                        (top_interface_layer && ! top_interface_layer->polygons.empty())) {
                        interface_layer = insert_layer(
                            intermediate_layer, polygons_bottom_contact_projected_interface, std::move(polygons_top_contact_projected_interface), top_interface_layer,
                            nullptr, polygons_top_contact_projected_interface.empty() ? SupporLayerType::BottomInterface : SupporLayerType::TopInterface);
                        interface_layers[idx_intermediate_layer] = interface_layer;
                    }
                    if (! polygons_bottom_contact_projected_base.empty() || ! polygons_top_contact_projected_base.empty() ||
                        (top_base_interface_layer && ! top_base_interface_layer->polygons.empty()))
                        base_interface_layers[idx_intermediate_layer] = insert_layer(
                            intermediate_layer, polygons_bottom_contact_projected_base, std::move(polygons_top_contact_projected_base), top_base_interface_layer,
                            interface_layer ? &interface_layer->polygons : nullptr, SupporLayerType::Base);
                }
            });

        // Compress contact_out, remove the nullptr items.
        // The parallel_for above may not have merged all the interface and base_interface layers
        // generated by the Organic supports code, do it here.
        auto merge_remove_empty = [](SupportGeneratorLayersPtr &in1, SupportGeneratorLayersPtr &in2) {
            std::set<const SupportGeneratorLayer *> seen;
            auto remove_empty = [&seen](SupportGeneratorLayersPtr &vec) {
                vec.erase(
                    std::remove_if(vec.begin(), vec.end(), [&seen](const SupportGeneratorLayer *ptr) {
                        // insert_layer may reuse a precomputed organic layer.
                        // It then occurs in both inputs, but still has one owner.
                        return ptr == nullptr || ptr->polygons.empty() || !seen.insert(ptr).second;
                    }),
                    vec.end());
            };
            remove_empty(in1);
            remove_empty(in2);
            if (in2.empty())
                return std::move(in1);
            else if (in1.empty())
                return std::move(in2);
            else {
                SupportGeneratorLayersPtr out(in1.size() + in2.size(), nullptr);
                std::merge(in1.begin(), in1.end(), in2.begin(), in2.end(), out.begin(), [](auto* l, auto* r) { return l->print_z < r->print_z; });
                return out;
            }
        };
        interface_layers      = merge_remove_empty(interface_layers,      top_interface_layers);
        base_interface_layers = merge_remove_empty(base_interface_layers, top_base_interface_layers);
        BOOST_LOG_TRIVIAL(debug) << "PrintObjectSupportMaterial::generate_interface_layers() in parallel - end";
        BOOST_LOG_TRIVIAL(info) << "Organic interface profiling: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - profile_started).count() << " seconds";
    }

    return base_and_interface_layers;
}

SupportGeneratorLayersPtr generate_raft_base(
    const PrintObject                 &object,
    const SupportParameters           &support_params,
	const SlicingParameters			  &slicing_params,
    const SupportGeneratorLayersPtr   &top_contacts,
    const SupportGeneratorLayersPtr   &interface_layers,
    const SupportGeneratorLayersPtr   &base_interface_layers,
    const SupportGeneratorLayersPtr   &base_layers,
    SupportGeneratorLayerStorage      &layer_storage)
{
    // If there is brim to be generated, calculate the trimming regions.
    Polygons brim;
    if (object.has_brim()) {
        // The object does not have a raft.
        // Calculate the area covered by the brim.
        const BrimType brim_type       = object.config().brim_type;
        // btLeadingEdgeOnly only means anything on a belt printer, where this code path
        // does not run; elsewhere it degrades to an outer brim (see Brim.cpp).
        const bool     brim_outer      = brim_type == btOuterOnly || brim_type == btOuterAndInner || brim_type == btLeadingEdgeOnly;
        const bool     brim_inner      = brim_type == btInnerOnly || brim_type == btOuterAndInner;
        // BBS: the pattern of raft and brim are the same, thus the brim can be serpated by support raft.
        const auto     brim_object_gap = scaled<float>(object.config().brim_object_gap.value);
        //const auto     brim_object_gap = scaled<float>(object.config().brim_object_gap.value + object.config().brim_width.value);
        for (const ExPolygon &ex : object.layers().front()->lslices) {
            if (brim_outer && brim_inner)
                polygons_append(brim, offset(ex, brim_object_gap));
            else {
                if (brim_outer)
                    polygons_append(brim, offset(ex.contour, brim_object_gap, jtRound, float(scale_(0.1))));
                else
                    brim.emplace_back(ex.contour);
                if (brim_inner) {
                    Polygons holes = ex.holes;
                    polygons_reverse(holes);
                    holes = shrink(holes, brim_object_gap, jtRound, float(scale_(0.1)));
                    polygons_reverse(holes);
                    polygons_append(brim, std::move(holes));
                } else
                    polygons_append(brim, ex.holes);
            }
        }
        brim = union_(brim);
    }

    // How much to inflate the support columns to be stable. This also applies to the 1st layer, if no raft layers are to be printed.
    const float inflate_factor_fine      = float(scale_((slicing_params.raft_layers() > 1) ? 0.5 : EPSILON));
    // On a belt the first support layer is the leading tip of the support, a sliver
    // where the belt crosses the layer, not a flange on a flat bed: inflating it
    // puts lines in the air ahead of the belt crossing (and into the belt behind
    // it).  The belt brim takes the adhesion role instead.
    const bool  belt_floor_active        = std::abs(slicing_params.belt_floor_shear_factor) > EPSILON;
    const float inflate_factor_1st_layer = belt_floor_active ? 0.f :
        std::max(0.f, float(scale_(object.config().raft_first_layer_expansion)) - inflate_factor_fine);
    SupportGeneratorLayer       *contacts         = top_contacts         .empty() ? nullptr : top_contacts         .front();
    SupportGeneratorLayer       *interfaces       = interface_layers     .empty() ? nullptr : interface_layers     .front();
    SupportGeneratorLayer       *base_interfaces  = base_interface_layers.empty() ? nullptr : base_interface_layers.front();
    SupportGeneratorLayer       *columns_base     = base_layers          .empty() ? nullptr : base_layers          .front();
    if (contacts != nullptr && contacts->print_z > std::max(slicing_params.first_print_layer_height, slicing_params.raft_contact_top_z) + EPSILON)
        // This is not the raft contact layer.
        contacts = nullptr;
    if (interfaces != nullptr && interfaces->bottom_print_z() > slicing_params.raft_interface_top_z + EPSILON)
        // This is not the raft column base layer.
        interfaces = nullptr;
    if (base_interfaces != nullptr && base_interfaces->bottom_print_z() > slicing_params.raft_interface_top_z + EPSILON)
        // This is not the raft column base layer.
        base_interfaces = nullptr;
    if (columns_base != nullptr && columns_base->bottom_print_z() > slicing_params.raft_interface_top_z + EPSILON)
        // This is not the raft interface layer.
        columns_base = nullptr;

    Polygons interface_polygons;
    if (contacts != nullptr && ! contacts->polygons.empty())
        polygons_append(interface_polygons, expand(contacts->polygons, inflate_factor_fine, SUPPORT_SURFACES_OFFSET_PARAMETERS));
    if (interfaces != nullptr && ! interfaces->polygons.empty())
        polygons_append(interface_polygons, expand(interfaces->polygons, inflate_factor_fine, SUPPORT_SURFACES_OFFSET_PARAMETERS));
    if (base_interfaces != nullptr && ! base_interfaces->polygons.empty())
        polygons_append(interface_polygons, expand(base_interfaces->polygons, inflate_factor_fine, SUPPORT_SURFACES_OFFSET_PARAMETERS));

    // Output vector.
    SupportGeneratorLayersPtr raft_layers;
    auto apply_layer_expansion = [&object, &slicing_params](Polygons polygons, size_t layer_id) {
        const coordf_t expansion = raft_layer_expansion_offset(
            object.config(), slicing_params.raft_layers(), layer_id);
        return expansion > EPSILON ? expand(polygons, scaled<float>(expansion)) : polygons;
    };

    if (slicing_params.raft_layers() > 1) {
        Polygons base;
        Polygons columns;
        Polygons first_layer;
        if (columns_base != nullptr) {
            if (columns_base->bottom_print_z() > slicing_params.raft_interface_top_z - EPSILON) {
                // Classic supports with colums above the raft interface.
                base = columns_base->polygons;
                columns = base;
                if (! interface_polygons.empty())
                    // Trim the 1st layer columns with the inflated interface polygons.
                    columns = diff(columns, interface_polygons);
            } else {
                // Organic supports with raft on print bed.
                assert(is_approx(columns_base->print_z, slicing_params.first_print_layer_height));
                first_layer = columns_base->polygons;
            }
        }
        if (! interface_polygons.empty()) {
            // Merge the untrimmed columns base with the expanded raft interface, to be used for the support base and interface.
            base = union_(base, interface_polygons);
        }
        // Do not add the raft contact layer, only add the raft layers below the contact layer.
        // Insert the 1st layer.
        {
            SupportGeneratorLayer &new_layer = layer_storage.allocate(slicing_params.base_raft_layers > 0 ? SupporLayerType::RaftBase : SupporLayerType::RaftInterface);
            raft_layers.push_back(&new_layer);
            new_layer.print_z = slicing_params.first_print_layer_height;
            new_layer.height  = slicing_params.first_print_layer_height;
            new_layer.bottom_z = 0.;
            first_layer = union_(std::move(first_layer), base);
            new_layer.polygons = inflate_factor_1st_layer > 0 ? expand(first_layer, inflate_factor_1st_layer) : first_layer;
            new_layer.polygons = apply_layer_expansion(std::move(new_layer.polygons), 0);
        }
        // Insert the base layers.
        for (size_t i = 1; i < slicing_params.base_raft_layers; ++ i) {
            coordf_t print_z = raft_layers.back()->print_z;
            SupportGeneratorLayer &new_layer  = layer_storage.allocate_unguarded(SupporLayerType::RaftBase);
            raft_layers.push_back(&new_layer);
            new_layer.print_z  = print_z + slicing_params.base_raft_layer_height;
            new_layer.height   = slicing_params.base_raft_layer_height;
            new_layer.bottom_z = print_z;
            new_layer.polygons = apply_layer_expansion(base, i);
        }
        // Insert the interface layers.
        for (size_t i = 1; i < slicing_params.interface_raft_layers; ++ i) {
            coordf_t print_z = raft_layers.back()->print_z;
            SupportGeneratorLayer &new_layer = layer_storage.allocate_unguarded(SupporLayerType::RaftInterface);
            raft_layers.push_back(&new_layer);
            new_layer.print_z = print_z + slicing_params.interface_raft_layer_height;
            new_layer.height  = slicing_params.interface_raft_layer_height;
            new_layer.bottom_z = print_z;
            const size_t layer_id = slicing_params.base_raft_layers + i - 1;
            new_layer.polygons = apply_layer_expansion(interface_polygons, layer_id);
            //FIXME misusing contact_polygons for support columns.
            new_layer.contact_polygons = std::make_unique<Polygons>(columns);
        }
    } else {
        if (columns_base != nullptr) {
            // Expand the bases of the support columns in the 1st layer.
            Polygons &raft     = columns_base->polygons;
            Polygons  trimming;
            // BBS: if first layer of support is intersected with object island, it must have the same function as brim unless in nobrim mode.
            // brim_object_gap is changed to 0 by default, it's no longer appropriate to use it to determine the gap of first layer support.
            trimming = offset(object.layers().front()->lslices, (float) scale_(support_params.gap_xy_first_layer), SUPPORT_SURFACES_OFFSET_PARAMETERS);
            if (inflate_factor_1st_layer > SCALED_EPSILON) {
                // Inflate in multiple steps to avoid leaking of the support 1st layer through object walls.
                auto  nsteps = std::max(5, int(ceil(inflate_factor_1st_layer / support_params.first_layer_flow.scaled_width())));
                float step   = inflate_factor_1st_layer / nsteps;
                for (int i = 0; i < nsteps; ++ i)
                    raft = diff(expand(raft, step), trimming);
            } else
                raft = diff(raft, trimming);
            if (! interface_polygons.empty())
                columns_base->polygons = diff(columns_base->polygons, interface_polygons);
        }
        if (! brim.empty()) {
            if (columns_base)
                columns_base->polygons = diff(columns_base->polygons, brim);
            if (contacts)
                contacts->polygons = diff(contacts->polygons, brim);
            if (interfaces)
                interfaces->polygons = diff(interfaces->polygons, brim);
            if (base_interfaces)
                base_interfaces->polygons = diff(base_interfaces->polygons, brim);
        }
    }

    return raft_layers;
}

static inline void fill_expolygon_generate_paths(
    ExtrusionEntitiesPtr    &dst,
    ExPolygon              &&expolygon,
    Fill                    *filler,
    const FillParams        &fill_params,
    float                    density,
    ExtrusionRole            role,
    const Flow              &flow)
{
    Surface surface(stInternal, std::move(expolygon));
    Polylines polylines;
    try {
        assert(!fill_params.use_arachne);
        polylines = filler->fill_surface(&surface, fill_params);
    } catch (InfillFailedException &) {
    }
    extrusion_entities_append_paths(
        dst,
        std::move(polylines),
        role,
        flow.mm3_per_mm(), flow.width(), flow.height());
}

static inline void fill_expolygons_generate_paths(
    ExtrusionEntitiesPtr    &dst,
    ExPolygons             &&expolygons,
    Fill                    *filler,
    const FillParams        &fill_params,
    float                    density,
    ExtrusionRole            role,
    const Flow              &flow)
{
    for (ExPolygon &expoly : expolygons)
        fill_expolygon_generate_paths(dst, std::move(expoly), filler, fill_params, density, role, flow);
}

static inline void fill_expolygons_generate_paths(
    ExtrusionEntitiesPtr    &dst,
    ExPolygons             &&expolygons,
    Fill                    *filler,
    float                    density,
    ExtrusionRole            role,
    const Flow              &flow,
    bool                     fill_concentric_gaps = false,
    bool                     preserve_short_paths = false,
    bool                     connect_concentric_loops = false)
{
    FillParams fill_params;
    fill_params.density               = density;
    fill_params.dont_adjust           = true;
    fill_params.fill_concentric_gaps  = fill_concentric_gaps;
    fill_params.preserve_short_paths  = preserve_short_paths;
    fill_params.connect_concentric_loops = connect_concentric_loops;
    if (fill_concentric_gaps)
        fill_params.flow = flow;
    fill_expolygons_generate_paths(dst, std::move(expolygons), filler, fill_params, density, role, flow);
}

// A clipped contact strip can be narrower than one normal infill line. Keep
// that strip printable with a variable-width medial-axis path instead of
// widening it to a full bead or dropping it with ordinary infill cleanup.
// This is intentionally used only for organic support contact/interface
// regions; regular model and base-support infill retain their normal filters.
static void append_narrow_contact_paths(
    ExtrusionEntitiesPtr &dst,
    const ExPolygons      &regions,
    size_t                 first_entity,
    ExtrusionRole          role,
    const Flow            &flow)
{
    if (regions.empty() || flow.scaled_width() <= 0 || first_entity > dst.size())
        return;

    Polygons covered;
    for (size_t idx = first_entity; idx < dst.size(); ++ idx)
        if (dst[idx] != nullptr)
            dst[idx]->polygons_covered_by_width(covered, 1.f);

    const float line_width = float(flow.scaled_width());
    // Identify narrow source geometry before subtracting the paths already
    // produced by the selected pattern. Otherwise a normal 0.05 mm spacing
    // between broad contact lines would look like a narrow leftover and be
    // incorrectly filled.
    const ExPolygons narrow_features = diff_ex(
        regions, opening_ex(regions, line_width / 2.f));
    if (narrow_features.empty())
        return;
    const ExPolygons remaining = covered.empty() ? regions :
        diff_ex(regions, union_ex(std::move(covered)));
    const ExPolygons narrow_remaining = intersection_ex(remaining, narrow_features);
    const coord_t width_correction = scaled<coord_t>(
        flow.height() * float(1. - 0.25 * PI));
    // The final printable-width check follows height modulation: a narrow
    // contact can become valid when an overlapping lower layer reduces height.
    const coord_t minimum_width = width_correction + SCALED_EPSILON;
    const coord_t maximum_width = std::max<coord_t>(
        minimum_width, line_width);
    // Wall/infill subtraction may leave nanometre-wide slivers or holes.
    // They cannot carry positive flow and can produce an invalid Voronoi
    // diagram. Opening at the minimum printable width only removes them;
    // it never widens the contact footprint or restores occupied material.
    for (const ExPolygon &region : opening_ex(narrow_remaining, 0.5f * float(minimum_width))) {

        ThickPolylines medial;
        region.medial_axis(
            minimum_width,
            maximum_width,
            &medial);
        for (ThickPolyline &path : medial) {
            // MedialAxis may accept a polyline when only one endpoint meets
            // min_width. Keep only paths whose complete bead can stay inside
            // the narrow source geometry, then compensate for the width term
            // that variable_width() adds for the rounded extrusion model.
            if (path.width.empty() || std::any_of(path.width.begin(), path.width.end(),
                [minimum_width](coordf_t width) { return width < minimum_width; })) {
                path.clear();
                continue;
            }
            for (coordf_t &width : path.width)
                width -= width_correction;
        }
        medial.erase(std::remove_if(medial.begin(), medial.end(),
            [](const ThickPolyline &path) { return !path.is_valid(); }), medial.end());
        if (!medial.empty())
            variable_width(medial, role, flow, dst);
    }
}

// Compact generic roof islands may use an original legal one-way arc.
// True contact pads retain their complete footprint for root handoff.
static Polylines compact_interface_source_arc(
    const Polyline &contour, const ExPolygons &regions, coord_t spacing, coord_t width)
{
    if (!contour.is_closed() || contour.points.size() < 4 || spacing <= 0 || width <= 0) return {};
    if (contour.length() > 4. * width) return {};
    Points points = contour.points;
    points.pop_back();
    const ExPolygon envelope(Geometry::convex_hull(points));
    if (!envelope.is_valid() || !offset_ex(envelope, -0.5f * float(spacing)).empty()) return {};
    const auto span = get_extents(points).size();
    const int axis = span.x() >= span.y() ? 0 : 1;
    const auto less = [axis](const Point &a, const Point &b) {
        return a[axis] < b[axis] || (a[axis] == b[axis] && a[1-axis] < b[1-axis]);
    };
    const size_t first = size_t(std::min_element(points.begin(), points.end(), less) - points.begin());
    const size_t last = size_t(std::max_element(points.begin(), points.end(), less) - points.begin());
    if (first == last) return {};
    const Polygons before = offset(contour, 0.5f * float(width),
        jtRound, double(SCALED_EPSILON), etOpenRound);
    Polylines result;
    double best = std::numeric_limits<double>::max();
    for (const int direction : {1, -1}) {
        Points arc{points[first]};
        for (size_t i = first; i != last;) {
            i = direction > 0 ? (i + 1) % points.size() : (i + points.size() - 1) % points.size();
            if (arc.back() != points[i]) arc.push_back(points[i]);
        }
        Polyline candidate(std::move(arc));
        candidate.simplify(SCALED_EPSILON);
        if (!candidate.is_valid() || !diff_pl(Polylines{candidate}, regions).empty()) continue;
        const Polygons after = offset(candidate, 0.5f * float(width),
            jtRound, double(SCALED_EPSILON), etOpenRound);
        const double lost = area(diff_ex(before, after));
        if (lost < best) { best = lost; result = Polylines{std::move(candidate)}; }
    }
    return result;
}

// A contour whose enclosed center domain cannot accommodate another half-pitch
// inset has no printable hollow interior. Its two return sides would overlap.
// Use one medial road at the original flow instead, retaining real source holes
// and every broad contour. This applies only to support interfaces, never trunks.
Polylines support_interface_single_road(
    const Polyline &contour, const ExPolygons &regions, coord_t spacing, coord_t width)
{
    if (!contour.is_closed() || contour.points.size() < 4 || spacing <= 0 || width <= 0)
        return {};
    Points points = contour.points;
    points.pop_back();
    Polygon polygon(std::move(points));
    polygon.make_counter_clockwise();
    ExPolygon center(std::move(polygon));
    // Projection during width normalization can make a tiny return contour
    // self-touch or retrace its own tail. Use its narrow envelope only when
    // that complete envelope still fits the original legal center domain.
    // A real hole or a broad/curved region therefore cannot be bridged.
    if (!center.is_valid() || offset_ex(center, -0.5f * float(spacing)).empty()) {
        ExPolygon envelope(Geometry::convex_hull(center.contour.points));
        if (envelope.is_valid() && offset_ex(envelope, -0.5f * float(spacing)).empty()) {
            if (diff_ex(ExPolygons{envelope}, regions).empty())
                center = std::move(envelope);
            else {
                // A valid bent strip can have a narrow hull that cuts across
                // its concave clearance. Keep the original curved center
                // domain, canonicalized before the Voronoi construction.
                ExPolygons legal = intersection_ex(opening_ex(intersection_ex(
                    union_ex(center.contour.simplify(2. * SCALED_EPSILON)), regions),
                    10.f * float(SCALED_EPSILON)), regions);
                if (legal.size() != 1 || !legal.front().holes.empty())
                    return {};
                center = std::move(legal.front());
            }
        }
    }
    if (!center.is_valid() || !offset_ex(center, -0.5f * float(spacing)).empty() ||
        !diff_ex(ExPolygons{center}, regions).empty())
        return {};
    Polylines medial;
    center.medial_axis(SCALED_EPSILON, 2. * spacing, &medial);
    medial.erase(std::remove_if(medial.begin(), medial.end(), [](const Polyline &line) {
        return !line.is_valid() || line.length() <= SCALED_EPSILON;
    }), medial.end());
    if (medial.size() > 1) {
        const double longest = std::max_element(medial.begin(), medial.end(),
            [](const Polyline &a, const Polyline &b) { return a.length() < b.length(); })->length();
        if (longest > width)
            medial.erase(std::remove_if(medial.begin(), medial.end(), [width](const Polyline &line) {
                return line.length() < width;
            }), medial.end());
    }
    return medial;
}

static void collapse_narrow_interface_contours(
    ExtrusionEntitiesPtr &dst, size_t first_entity, const ExPolygons &regions, const Flow &flow,
    bool compact_roof_arcs = false)
{
    ExtrusionEntitiesPtr result;
    const ExPolygons legal_centers = offset_ex(regions,
        -0.5f * float(flow.scaled_width()) + float(SCALED_EPSILON));
    for (size_t idx = first_entity; idx < dst.size(); ++idx) {
        auto *path = dynamic_cast<ExtrusionPath *>(dst[idx]);
        if (path == nullptr || path->role() != erSupportMaterialInterface || path->z_contoured ||
            path->first_point3() != path->last_point3() ||
            std::abs(path->width - flow.width()) > EPSILON) {
            result.push_back(dst[idx]);
            continue;
        }
        Polylines medial = support_interface_single_road(path->polyline.to_polyline(), legal_centers,
            flow.scaled_spacing(), flow.scaled_width());
        if (compact_roof_arcs) {
            medial.erase(std::remove_if(medial.begin(), medial.end(),
                [](const Polyline &line) { return line.is_closed(); }), medial.end());
            if (medial.empty())
                medial = compact_interface_source_arc(path->polyline.to_polyline(), legal_centers,
                    flow.scaled_spacing(), flow.scaled_width());
        }
        if (medial.empty()) { result.push_back(path); continue; }
        for (Polyline &line : medial)
            result.push_back(new ExtrusionPath(Polyline3(std::move(line)), *path));
        delete path;
    }
    dst.resize(first_entity);
    append(dst, std::move(result));
}

void fill_support_interface_with_walls(
    ExtrusionEntitiesPtr &dst,
    const ExPolygons     &regions,
    Fill                 *filler,
    const FillParams     &fill_params,
    ExtrusionRole         role,
    const Flow           &flow,
    int                   wall_count,
    bool                  preserve_narrow_regions,
    const ExPolygons     *wall_regions)
{
    if (regions.empty())
        return;
    FillParams effective_params = fill_params;
    if (fill_params.fill_concentric_gaps && fill_params.density > 0.) {
        // Concentric walls and their keepout use the actual layer's flow.
        // Using the nominal support-height spacing for the fill puts its
        // rings inside that keepout at independent, thinner contact planes.
        // Preserve the configured clear spacing while changing the reference
        // bead spacing; changing density alone would change the process gap.
        const double clear_spacing = std::max(0., filler->spacing * (1. / fill_params.density - 1.));
        filler->spacing = flow.spacing();
        effective_params.density = filler->spacing / (filler->spacing + clear_spacing);
    }
    const size_t first_entity = dst.size();
    wall_count = std::max(0, wall_count);
    if (wall_count == 0) wall_regions = nullptr;
    const float spacing = float(flow.scaled_spacing());
    const float half_width = 0.5f * float(flow.scaled_width());
    ExPolygons fill_regions;
    for (const ExPolygon &region : wall_regions == nullptr ? regions : *wall_regions) {
        int generated_walls = 0;
        ExPolygons inner_wall_centers;
        for (; generated_walls < wall_count; ++ generated_walls) {
            ExPolygons centers = offset_ex(region, -half_width - generated_walls * spacing);
            if (centers.empty())
                break;
            inner_wall_centers = centers;
            Polylines walls;
            for (const Polygon &polygon : to_polygons(std::move(centers)))
                walls.emplace_back(polygon.split_at_index(0));
            // Support height modulation consumes paths, not model-wall loop
            // entities. These paths remain closed and keep their support role.
            extrusion_entities_append_paths(dst, std::move(walls), role,
                flow.mm3_per_mm(), flow.width(), flow.height(), wall_regions == nullptr);
        }
        if (generated_walls == 0)
            fill_regions.push_back(region);
        else
            // Fill::fill_surface applies another half-spacing inset. Keep its
            // first centerline one normal spacing from the innermost wall;
            // do not print the original outer concentric ring a second time.
            // Derive fill from the wall actually printed. Independent offsets
            // of the original region can disagree along a rounded/concave
            // boundary and the later keepout then cuts a concentric ring apart.
            expolygons_append(fill_regions, offset_ex(inner_wall_centers,
                -0.5f * spacing - 2.f * SCALED_EPSILON));
    }
    // A stepped landing has one structural rim but several physical contact
    // patches. Its wall follows the complete parent section; only the true
    // patches receive the configured contact infill at this height.
    if (wall_regions != nullptr)
        fill_regions = intersection_ex(fill_regions, regions);
    const size_t first_fill = dst.size();
    const ExPolygons narrow_fill_regions = preserve_narrow_regions ? fill_regions : ExPolygons{};
    Polygons wall_keepout;
    if (first_fill > first_entity) {
        for (size_t idx = first_entity; idx < first_fill; ++idx)
            dst[idx]->polygons_covered_by_spacing(wall_keepout, 1.f);
        wall_keepout = offset(union_(wall_keepout), 0.5f * spacing - float(SCALED_EPSILON));
        // Plan links in the safe center domain, rather than connecting across
        // a concave wall and cutting that connection into isolated fragments.
        // Account for Fill::fill_surface's normal inset exactly once, keeping
        // the rectilinear phase and configured pitch unchanged.
        const float inset = float(scale_(filler->overlap - 0.5 * filler->spacing));
        fill_regions = offset_ex(diff_ex(offset_ex(fill_regions, inset), wall_keepout),
            -2.f * SCALED_EPSILON);
    }
    if (!wall_keepout.empty())
        effective_params.boundary_is_center_domain = true;
    fill_expolygons_generate_paths(dst, std::move(fill_regions), filler, effective_params,
        float(effective_params.density), role, flow);
    if (first_fill > first_entity && dst.size() > first_fill) {
        // Offset contours alone do not guarantee the normal wall/fill spacing
        // at concave corners: rectilinear end connections may cut that corner.
        // Clip only interior paths against the actual walls, not the footprint.
        ExtrusionEntitiesPtr clipped;
        for (size_t idx = first_fill; idx < dst.size(); ++idx) {
            const auto *path = dynamic_cast<const ExtrusionPath*>(dst[idx]);
            assert(path != nullptr);
            for (Polyline &fragment : diff_pl(Polylines{path->polyline.to_polyline()}, wall_keepout))
                if (fragment.is_valid())
                    clipped.emplace_back(new ExtrusionPath(Polyline3(std::move(fragment)), *path));
            delete dst[idx];
        }
        dst.resize(first_fill);
        append(dst, std::move(clipped));
    }
    if (fill_params.preserve_short_paths && role == erSupportMaterialInterface)
        collapse_narrow_interface_contours(dst, first_entity, regions, flow);
    if (preserve_narrow_regions) {
        // Preserve thin original contacts as well as an interior remainder
        // that collapses after the requested walls. Coverage includes the new
        // walls, so the fallback cannot duplicate their material.
        const size_t first_narrow = dst.size();
        append_narrow_contact_paths(dst, regions, first_entity, role, flow);
        append_narrow_contact_paths(dst, narrow_fill_regions, first_entity, role, flow);
        // variable_width() may return a closed ExtrusionLoop for a thin annulus.
        // Support height modulation requires paths; retain every segment's flow.
        ExtrusionEntitiesPtr narrow_paths;
        for (size_t idx = first_narrow; idx < dst.size(); ++idx)
            if (auto *loop = dynamic_cast<ExtrusionLoop*>(dst[idx])) {
                for (ExtrusionPath &path : loop->paths)
                    narrow_paths.emplace_back(new ExtrusionPath(std::move(path)));
                delete loop;
            } else
                narrow_paths.emplace_back(dst[idx]);
        dst.resize(first_narrow);
        append(dst, std::move(narrow_paths));
    }

    // A clipped interface can leave a positive-flow remnant shorter than the
    // printer's coordinate resolution. Keeping those remnants produces many
    // isolated one-segment islands on tree trunks; discard only new interface
    // paths below the fixed 0.1 mm minimum.
    if (role == ExtrusionRole::erSupportMaterialInterface) {
        const coord_t minimum_path_length = scale_(0.1);
        for (size_t idx = first_entity; idx < dst.size();) {
            auto *path = dynamic_cast<ExtrusionPath *>(dst[idx]);
            if (path != nullptr && path->length() < minimum_path_length) {
                delete dst[idx];
                dst.erase(dst.begin() + idx);
            } else
                ++idx;
        }
    }
}

static Polylines draw_perimeters(const ExPolygon &expoly, double clip_length)
{
    // Draw the perimeters.
    Polylines polylines;
    polylines.reserve(expoly.holes.size() + 1);
    for (size_t i = 0; i <= expoly.holes.size();  ++ i) {
        Polyline pl(i == 0 ? expoly.contour.points : expoly.holes[i - 1].points);
        pl.points.emplace_back(pl.points.front());
        if (i > 0)
            // It is a hole, reverse it.
            pl.reverse();
        // so that all contours are CCW oriented.
        pl.clip_end(clip_length);
        polylines.emplace_back(std::move(pl));
    }
    return polylines;
}

void tree_supports_generate_paths(
    ExtrusionEntitiesPtr    &dst,
    const Polygons          &polygons,
    const Flow              &flow,
    const SupportParameters &support_params)
{
    if (support_params.support_style == smsTreeOrganic) {
        // Organic trunks are complete contour roads. Avoid the generic seam
        // anchor, which can project into a hollow branch without a bead below.
        auto printable_contours = [&](const ExPolygon &region) {
            Polylines result = draw_perimeters(region, 0.);
            // A mesh-union pinhole smaller than one bead cannot have a
            // printable inner wall. Printing its perimeter creates an
            // isolated blob inside an otherwise hollow branch. Retain real
            // fork/model voids and every original outside contour.
            for (size_t idx = result.size(); idx-- > 1;) {
                const auto size = get_extents(result[idx]).size();
                if (size.x() < flow.scaled_width() && size.y() < flow.scaled_width())
                    result.erase(result.begin() + idx);
            }
            return result;
        };
        for (const ExPolygon &region : closing_ex(polygons, float(SCALED_EPSILON),
                float(SCALED_EPSILON))) {
            auto walls = std::make_unique<ExtrusionEntityCollection>();
            walls->no_sort = true;
            if (region.area() > support_params.tree_branch_diameter_double_wall_area_scaled)
                for (const ExPolygon &inner : offset2_ex({region}, -1.5 * flow.scaled_width(), 0.5 * flow.scaled_width()))
                    extrusion_entities_append_paths(walls->entities, printable_contours(inner),
                        ExtrusionRole::erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height(), false);
            extrusion_entities_append_paths(walls->entities, printable_contours(region),
                ExtrusionRole::erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height(), false);
            for (ExtrusionEntity *wall : walls->entities)
                wall->set_reverse();
            if (!walls->empty())
                dst.push_back(walls.release());
        }
        return;
    }
    // Offset expolygon inside, returns number of expolygons collected (0 or 1).
    // Vertices of output paths are marked with Z = source contour index of the expoly.
    // Vertices at the intersection of source contours are marked with Z = -1.
    auto shrink_expolygon_with_contour_idx = [](const Slic3r::ExPolygon &expoly, const float delta, JoinType joinType, double miterLimit, ClipperZUtils::ZPaths &out) -> int
    {
        assert(delta > 0);
        auto append_paths_with_z = [](const Polygons &src, coord_t contour_idx, ClipperZUtils::ZPaths &dst) {
            dst.reserve(next_highest_power_of_2(dst.size() + src.size()));
            for (const Polygon &contour : src)
                dst.emplace_back(ClipperZUtils::to_zpath(contour.points, contour_idx));
        };
        // Oriented CCW, the sign of d alone decides between growing and shrinking.
        auto offset_ccw = [joinType, miterLimit](Polygon polygon, float d) {
            if (! polygon.is_counter_clockwise())
                polygon.reverse();
            return offset(polygon, d, joinType, miterLimit);
        };

        // 1) Offset the outer contour.
        ClipperZUtils::ZPaths contours;
        {
            Polygons contours_raw = offset_ccw(expoly.contour, - delta);
            if (contours_raw.empty())
                // No need to try to offset the holes.
                return 0;
            append_paths_with_z(contours_raw, 0, contours);
        }

        if (expoly.holes.empty()) {
            // No need to subtract holes from the offsetted expolygon, we are done.
            append(out, std::move(contours));
        } else {
            // 2) Offset the holes one by one, collect the offsetted holes.
            ClipperZUtils::ZPaths holes;
            for (const Polygon &hole : expoly.holes)
                append_paths_with_z(offset_ccw(hole, delta), 1 + (&hole - expoly.holes.data()), holes);

            // 3) Subtract holes from the contours.
            if (holes.empty()) {
                // No hole remaining after an offset. Just copy the outer contour.
                append(out, std::move(contours));
            } else {
                // Negative offset. There is a chance, that the offsetted hole intersects the outer contour.
                // Subtract the offsetted holes from the offsetted contours.
                ClipperZUtils::ZPaths output = ClipperZUtils::clip_zpaths(ctDifference, contours, false, holes,
                    [](const ClipperZUtils::ZPoint &, const ClipperZUtils::ZPoint &, const ClipperZUtils::ZPoint &, const ClipperZUtils::ZPoint &, ClipperZUtils::ZPoint &pt) {
                        // Just mark the intersection.
                        pt.z() = -1;
                    });
                if (! output.empty()) {
                    append(out, std::move(output));
                } else {
                    // The offsetted holes have eaten up the offsetted outer contour.
                    return 0;
                }
            }
        }

        return 1;
    };

    const double spacing = flow.scaled_spacing();
    // Clip the sheath path to avoid the extruder to get exactly on the first point of the loop.
    const double clip_length = support_params.support_style == smsTreeOrganic ? 0. : spacing * 0.15;
    const double anchor_length = spacing * 6.;
    ClipperZUtils::ZPaths anchor_candidates;
    for (ExPolygon& expoly : closing_ex(polygons, float(SCALED_EPSILON), float(SCALED_EPSILON + 0.5 * flow.scaled_width()))) {
        std::unique_ptr<ExtrusionEntityCollection> eec;
        ExPolygons                                 regions_to_draw_inner_wall{expoly};
        if (support_params.tree_branch_diameter_double_wall_area_scaled > 0)
            if (double area = expoly.area(); area > support_params.tree_branch_diameter_double_wall_area_scaled) {
                BOOST_LOG_TRIVIAL(debug)<< "TreeSupports: double wall area: " << area<< " > " << support_params.tree_branch_diameter_double_wall_area_scaled;
                eec = std::make_unique<ExtrusionEntityCollection>();
                // Don't reorder internal / external loops of the same island, always start with the internal loop.
                eec->no_sort = true;
                // Make the tree branch stable by adding another perimeter.
                ExPolygons level2 = offset2_ex({expoly}, -1.5 * flow.scaled_width(), 0.5 * flow.scaled_width());
                if (level2.size() > 0) {
                    regions_to_draw_inner_wall = level2;
                    extrusion_entities_append_paths(eec->entities, draw_perimeters(expoly, clip_length), ExtrusionRole::erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height(),
                            // Disable reversal of the path, always start with the anchor, always print CCW.
                            false);
                    expoly = level2.front();
                }
            }
        for (ExPolygon &expoly : regions_to_draw_inner_wall)
        {
            // Try to produce one more perimeter to place the seam anchor.
            // First genrate a 2nd perimeter loop as a source for anchor candidates.
            // The anchor candidate points are annotated with an index of the source contour or with -1 if on intersection.
            anchor_candidates.clear();
            shrink_expolygon_with_contour_idx(expoly, flow.scaled_width(), jtMiter, 1.2, anchor_candidates);
            // Orient all contours CW.
            for (auto &path : anchor_candidates)
                if (ClipperZUtils::area(path) > 0) std::reverse(path.begin(), path.end());

            // Draw the perimeters.
            Polylines polylines;
            polylines.reserve(expoly.holes.size() + 1);
            for (int idx_loop = 0; idx_loop < int(expoly.num_contours()); ++idx_loop) {
                // Open the loop with a seam.
                const Polygon &loop = expoly.contour_or_hole(idx_loop);
                Polyline       pl(loop.points);
                // Orient all contours CW, because the anchor will be added to the end of polyline while we want to start a loop with the anchor.
                if (idx_loop == 0)
                    // It is an outer contour.
                    pl.reverse();
                pl.points.emplace_back(pl.points.front());
                pl.clip_end(clip_length);
                if (pl.size() < 2) continue;
                // Find the foot of the seam point on anchor_candidates. Only pick an anchor point that was created by offsetting the source contour.
                ClipperZUtils::ZPath *closest_contour = nullptr;
                Vec2d                 closest_point;
                int                   closest_point_idx = -1;
                double                closest_point_t   = 0.;
                double                d2min             = std::numeric_limits<double>::max();
                Vec2d                 seam_pt           = pl.back().cast<double>();
                for (ClipperZUtils::ZPath &path : anchor_candidates)
                    for (int i = 0; i < int(path.size()); ++i) {
                        int j = next_idx_modulo(i, path);
                        if (path[i].z() == idx_loop || path[j].z() == idx_loop) {
                            Vec2d pi(path[i].x(), path[i].y());
                            Vec2d pj(path[j].x(), path[j].y());
                            Vec2d v  = pj - pi;
                            Vec2d w  = seam_pt - pi;
                            auto  l2 = v.squaredNorm();
                            auto  t  = std::clamp((l2 == 0) ? 0 : v.dot(w) / l2, 0., 1.);
                            if ((path[i].z() == idx_loop || t > EPSILON) && (path[j].z() == idx_loop || t < 1. - EPSILON)) {
                                // Closest point.
                                Vec2d  fp = pi + v * t;
                                double d2 = (fp - seam_pt).squaredNorm();
                                if (d2 < d2min) {
                                    d2min             = d2;
                                    closest_contour   = &path;
                                    closest_point     = fp;
                                    closest_point_idx = i;
                                    closest_point_t   = t;
                                }
                            }
                        }
                    }
                if (d2min < sqr(flow.scaled_width() * 3.)) {
                    // Try to cut an anchor from the closest_contour.
                    // Both closest_contour and pl are CW oriented.
                    pl.points.emplace_back(closest_point.cast<coord_t>());
                    const ClipperZUtils::ZPath &path             = *closest_contour;
                    double                      remaining_length = anchor_length - (seam_pt - closest_point).norm();
                    int                         i                = closest_point_idx;
                    int                         j                = next_idx_modulo(i, *closest_contour);
                    Vec2d                       pi(path[i].x(), path[i].y());
                    Vec2d                       pj(path[j].x(), path[j].y());
                    Vec2d                       v = pj - pi;
                    double                      l = v.norm();
                    if (remaining_length < (1. - closest_point_t) * l) {
                        // Just trim the current line.
                        pl.points.emplace_back((closest_point + v * (remaining_length / l)).cast<coord_t>());
                    } else {
                        // Take the rest of the current line, continue with the other lines.
                        pl.points.emplace_back(path[j].x(), path[j].y());
                        pi = pj;
                        for (i = j; path[i].z() == idx_loop && remaining_length > 0; i = j, pi = pj) {
                            j  = next_idx_modulo(i, path);
                            pj = Vec2d(path[j].x(), path[j].y());
                            v  = pj - pi;
                            l  = v.norm();
                            if (i == closest_point_idx) {
                                // Back at the first segment. Most likely this should not happen and we may end the anchor.
                                break;
                            }
                            if (remaining_length <= l) {
                                pl.points.emplace_back((pi + v * (remaining_length / l)).cast<coord_t>());
                                break;
                            }
                            pl.points.emplace_back(path[j].x(), path[j].y());
                            remaining_length -= l;
                        }
                    }
                }
                // Start with the anchor.
                pl.reverse();
                polylines.emplace_back(std::move(pl));
            }

            ExtrusionEntitiesPtr &out = eec ? eec->entities : dst;
            extrusion_entities_append_paths(out, std::move(polylines), ExtrusionRole::erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height(),
                                            // Disable reversal of the path, always start with the anchor, always print CCW.
                                            false);
        }
        if (eec) {
            if (!eec->empty()) {
                std::reverse(eec->entities.begin(), eec->entities.end());
                dst.emplace_back(eec.release());
            }
        }
    }
}

void fill_expolygons_with_sheath_generate_paths(
    ExtrusionEntitiesPtr    &dst,
    const Polygons          &polygons,
    Fill                    *filler,
    float                    density,
    ExtrusionRole            role,
    const Flow              &flow,
    const SupportParameters& support_params,
    bool                     with_sheath,
    bool                     no_sort,
    bool                     fill_concentric_gaps,
    bool                     connect_support_zigzag,
    bool                     respect_extrusion_width)
{
    if (polygons.empty())
        return;

    FillParams fill_params;
    fill_params.density              = density;
    fill_params.dont_adjust          = true;
    fill_params.fill_concentric_gaps = fill_concentric_gaps;
    if (fill_concentric_gaps)
        fill_params.flow = flow;
    fill_params.connect_support_zigzag = connect_support_zigzag;

    if (with_sheath) {
        if (density == 0) {
            tree_supports_generate_paths(dst, polygons, flow, support_params);
            return;
        }
    }
    else {
        ExPolygons fill_regions = closing_ex(polygons, float(SCALED_EPSILON));
        if (respect_extrusion_width) {
            // Fill::fill_surface() insets by half the reference line spacing.
            // Keep that spacing for aligned support rows, but reserve the
            // complete bead width at the collision-trimmed boundary.
            const double extra_inset = std::max(0., 0.5 * (flow.width() - filler->spacing));
            if (extra_inset > EPSILON)
                fill_regions = offset_ex(fill_regions, float(-scale_(extra_inset)));
        }
        fill_expolygons_generate_paths(dst, std::move(fill_regions), filler, fill_params, density, role, flow);
        return;
    }

    const double spacing = flow.scaled_spacing();
    // Clip the sheath path to avoid the extruder to get exactly on the first point of the loop.
    const double clip_length = support_params.support_style == smsTreeOrganic ? 0. : spacing * 0.15;

    for (ExPolygon &expoly : closing_ex(polygons, float(SCALED_EPSILON), float(SCALED_EPSILON + 0.5*flow.scaled_width()))) {
        // Don't reorder the skirt and its infills.
        std::unique_ptr<ExtrusionEntityCollection> eec;
        if (no_sort) {
            eec = std::make_unique<ExtrusionEntityCollection>();
            eec->no_sort = true;
        }
        ExtrusionEntitiesPtr &out = no_sort ? eec->entities : dst;
        extrusion_entities_append_paths(out, draw_perimeters(expoly, clip_length), ExtrusionRole::erSupportMaterial, flow.mm3_per_mm(), flow.width(), flow.height());
        // Fill in the rest.
        fill_expolygons_generate_paths(out, offset_ex(expoly, float(-0.4 * spacing)), filler, fill_params, density, role, flow);
        if (no_sort && ! eec->empty())
            dst.emplace_back(eec.release());
    }
}

namespace {

using RaftExtrusionSignature = std::tuple<int, int64_t, int64_t, int64_t>;

int64_t quantize_raft_value(double value)
{
    return int64_t(std::llround(value * 1'000'000.));
}

bool collect_raft_path_signatures(
    const ExtrusionEntity              &entity,
    std::set<RaftExtrusionSignature>   &signatures,
    std::set<ExtrusionRole>            &roles)
{
    auto collect_path = [&signatures, &roles](const ExtrusionPath &path) {
        roles.insert(path.role());
        signatures.emplace(
            int(path.role()), quantize_raft_value(path.mm3_per_mm),
            quantize_raft_value(path.width), quantize_raft_value(path.height));
    };

    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            if (child == nullptr || !collect_raft_path_signatures(*child, signatures, roles))
                return false;
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
        collect_path(*path);
    } else if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
        for (const ExtrusionPath &path : multipath->paths)
            collect_path(path);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
        for (const ExtrusionPath &path : loop->paths)
            collect_path(path);
    } else {
        return false;
    }
    return true;
}

bool raft_path_signatures(
    const ExtrusionEntityCollection    &paths,
    std::set<RaftExtrusionSignature>   &signatures,
    std::set<ExtrusionRole>            &roles)
{
    return collect_raft_path_signatures(paths, signatures, roles) &&
        std::all_of(roles.begin(), roles.end(), [](ExtrusionRole role) {
            return role == erSupportMaterial || role == erSupportMaterialInterface;
        });
}

bool raft_instances_compatible(const RaftPathInstance &lhs, const RaftPathInstance &rhs)
{
    if (lhs.object == nullptr || rhs.object == nullptr || lhs.layer == nullptr || rhs.layer == nullptr)
        return false;
    if (lhs.layer->id() != rhs.layer->id() ||
        std::abs(lhs.layer->height - rhs.layer->height) > EPSILON ||
        std::abs(lhs.layer->print_z - rhs.layer->print_z) > EPSILON ||
        lhs.support_extruder != rhs.support_extruder ||
        lhs.interface_extruder != rhs.interface_extruder)
        return false;

    const PrintObjectConfig &lhs_config = lhs.object->config();
    const PrintObjectConfig &rhs_config = rhs.object->config();
    const SlicingParameters &lhs_slicing = lhs.object->slicing_parameters();
    const SlicingParameters &rhs_slicing = rhs.object->slicing_parameters();
    if (lhs_slicing.base_raft_layers != rhs_slicing.base_raft_layers ||
        lhs_slicing.interface_raft_layers != rhs_slicing.interface_raft_layers ||
        lhs_config.raft_base_pattern.value != rhs_config.raft_base_pattern.value ||
        lhs_config.support_style.value != rhs_config.support_style.value ||
        lhs_config.support_filament.value != rhs_config.support_filament.value ||
        lhs_config.support_interface_filament.value != rhs_config.support_interface_filament.value ||
        lhs_config.tree_support_wall_count.value != rhs_config.tree_support_wall_count.value ||
        lhs_config.support_ironing.value != rhs_config.support_ironing.value ||
        std::abs(lhs_config.raft_base_pattern_spacing.value - rhs_config.raft_base_pattern_spacing.value) > EPSILON ||
        std::abs(lhs_config.raft_first_layer_density.value - rhs_config.raft_first_layer_density.value) > EPSILON ||
        std::abs(lhs_config.raft_layer_expansion_step.value - rhs_config.raft_layer_expansion_step.value) > EPSILON ||
        std::abs(lhs_config.support_angle.value - rhs_config.support_angle.value) > EPSILON)
        return false;

    std::set<RaftExtrusionSignature> lhs_signatures;
    std::set<RaftExtrusionSignature> rhs_signatures;
    std::set<ExtrusionRole> lhs_roles;
    std::set<ExtrusionRole> rhs_roles;
    return lhs.paths != nullptr && rhs.paths != nullptr &&
        raft_path_signatures(*lhs.paths, lhs_signatures, lhs_roles) &&
        raft_path_signatures(*rhs.paths, rhs_signatures, rhs_roles) &&
        lhs_signatures == rhs_signatures && lhs_roles == rhs_roles;
}

struct RaftRoleRegions
{
    ExPolygons support;
    ExPolygons interface;
};

bool get_raft_role_regions(const RaftPathInstance &instance, RaftRoleRegions &regions)
{
    if (instance.layer == nullptr || instance.paths == nullptr)
        return false;

    std::set<RaftExtrusionSignature> signatures;
    std::set<ExtrusionRole> roles;
    if (!raft_path_signatures(*instance.paths, signatures, roles))
        return false;

    regions.support = instance.layer->raft_support_islands;
    regions.interface = instance.layer->raft_interface_islands;
    if (regions.support.empty() && regions.interface.empty()) {
        ExPolygons footprint = instance.footprint == nullptr ? ExPolygons{} : *instance.footprint;
        if (footprint.empty())
            footprint = union_ex(instance.paths->polygons_covered_by_spacing());
        if (footprint.empty())
            return false;

        if (roles.size() == 1 && *roles.begin() == erSupportMaterial)
            regions.support = std::move(footprint);
        else if (roles.size() == 1 && *roles.begin() == erSupportMaterialInterface)
            regions.interface = std::move(footprint);
        else if (instance.layer->id() == 0 && roles.size() == 2 &&
                 roles.count(erSupportMaterial) != 0 && roles.count(erSupportMaterialInterface) != 0)
            // A one-layer raft uses the support role for its outer sheath and
            // the interface role for the fill. Regenerating the interface
            // region with a sheath reproduces both roles.
            regions.interface = std::move(footprint);
        else
            return false;
    }

    if (regions.support.empty() && roles.count(erSupportMaterial) != 0 &&
        !(instance.layer->id() == 0 && !regions.interface.empty()))
        return false;
    if (regions.interface.empty() && roles.count(erSupportMaterialInterface) != 0)
        return false;
    return true;
}

bool is_nonorganic_tree_style(SupportMaterialStyle style)
{
    return style == smsTreeSlim || style == smsTreeStrong || style == smsTreeHybrid;
}

void generate_merged_raft_region(
    ExtrusionEntityCollection  &dst,
    const ExPolygons           &regions,
    ExtrusionRole               role,
    const PrintObject          &object,
    const SupportLayer         &layer)
{
    if (regions.empty())
        return;

    const PrintObjectConfig &config = object.config();
    const SlicingParameters &slicing_params = object.slicing_parameters();
    const SupportParameters support_params(object);
    const bool first_layer = layer.id() == 0;
    const bool interface_role = role == erSupportMaterialInterface;
    const bool nonorganic_tree = is_nonorganic_tree_style(support_params.support_style);

    const InfillPattern pattern = interface_role ?
        support_params.raft_interface_fill_pattern : support_params.raft_base_fill_pattern;
    std::unique_ptr<Fill> filler(Fill::new_from_type(pattern));
    filler->set_bounding_box(get_extents(regions));
    filler->layer_id = layer.id();
    filler->z = layer.print_z;

    Flow flow = first_layer ? support_params.first_layer_flow :
        (interface_role ? support_params.raft_interface_flow : support_params.support_material_flow)
            .with_height(float(layer.height));
    float density = first_layer ? float(config.raft_first_layer_density.value * 0.01) :
        float(interface_role ? support_params.raft_interface_density : support_params.raft_base_density);

    if (first_layer) {
        filler->angle = nonorganic_tree ? float(M_PI_2) : support_params.raft_angle_1st_layer;
        filler->spacing = flow.spacing();
    } else if (interface_role) {
        filler->angle = nonorganic_tree ? float(M_PI_2) :
            support_params.raft_interface_angle(layer.interface_id());
        filler->spacing = nonorganic_tree ? flow.spacing() : support_params.raft_interface_flow.spacing();
    } else if (nonorganic_tree) {
        filler->angle = layer.id() < slicing_params.base_raft_layers ? 0.f : float(M_PI_2);
        filler->spacing = flow.spacing();
        if (layer.id() >= slicing_params.base_raft_layers)
            density = layer.id() < slicing_params.base_raft_layers + slicing_params.interface_raft_layers ?
                float(config.raft_first_layer_density.value * 0.01) : float(support_params.raft_interface_density);
    } else {
        filler->angle = support_params.raft_angle_base;
        if (config.raft_base_pattern == smpRectilinearGrid && (layer.id() & 1) != 0)
            filler->angle = support_params.raft_angle_interface;
        filler->spacing = support_params.support_material_flow.spacing();
    }

    Polygons polygons = to_polygons(regions);
    fill_expolygons_with_sheath_generate_paths(
        dst.entities, polygons, filler.get(), density, role, flow,
        support_params, first_layer, first_layer, pattern == ipConcentric);
}

std::unique_ptr<ExtrusionEntityCollection> generate_merged_raft_paths(
    const RaftPathInstance &owner,
    ExPolygons              support_regions,
    ExPolygons              interface_regions)
{
    if (owner.object == nullptr || owner.layer == nullptr)
        return nullptr;
    if (owner.layer->id() == 0 && !support_regions.empty() && !interface_regions.empty())
        return nullptr;

    auto result = std::make_unique<ExtrusionEntityCollection>();
    result->no_sort = owner.paths != nullptr && owner.paths->no_sort;
    generate_merged_raft_region(*result, support_regions, erSupportMaterial, *owner.object, *owner.layer);
    generate_merged_raft_region(*result, interface_regions, erSupportMaterialInterface, *owner.object, *owner.layer);
    return result->empty() ? nullptr : std::move(result);
}

} // namespace

std::vector<std::unique_ptr<ExtrusionEntityCollection>> merge_overlapping_raft_paths(
    const std::vector<RaftPathInstance> &instances)
{
    std::vector<std::unique_ptr<ExtrusionEntityCollection>> result(instances.size());
    if (instances.size() < 2)
        return result;

    std::vector<ExPolygons> global_footprints(instances.size());
    std::vector<BoundingBox> global_footprint_bboxes(instances.size());
    for (size_t index = 0; index < instances.size(); ++index) {
        const RaftPathInstance &instance = instances[index];
        global_footprints[index] = instance.footprint == nullptr ? ExPolygons{} : *instance.footprint;
        if (global_footprints[index].empty() && instance.paths != nullptr)
            global_footprints[index] = union_ex(instance.paths->polygons_covered_by_spacing());
        translate(global_footprints[index], instance.shift);
        global_footprint_bboxes[index] = get_extents(global_footprints[index]);
    }

    std::vector<size_t> parent(instances.size());
    std::iota(parent.begin(), parent.end(), size_t(0));
    auto find_root = [&parent](size_t index) {
        while (parent[index] != index) {
            parent[index] = parent[parent[index]];
            index = parent[index];
        }
        return index;
    };
    auto unite = [&parent, &find_root](size_t lhs, size_t rhs) {
        lhs = find_root(lhs);
        rhs = find_root(rhs);
        if (lhs != rhs)
            parent[rhs] = lhs;
    };

    for (size_t lhs = 0; lhs < instances.size(); ++lhs)
        for (size_t rhs = lhs + 1; rhs < instances.size(); ++rhs)
            if (global_footprint_bboxes[lhs].defined && global_footprint_bboxes[rhs].defined &&
                global_footprint_bboxes[lhs].overlap(global_footprint_bboxes[rhs]) &&
                !intersection_ex(global_footprints[lhs], global_footprints[rhs]).empty())
                unite(lhs, rhs);

    std::map<size_t, std::vector<size_t>> components;
    for (size_t index = 0; index < instances.size(); ++index)
        components[find_root(index)].push_back(index);

    for (const auto &[root, members] : components) {
        (void) root;
        if (members.size() < 2)
            continue;

        const size_t owner_index = members.front();
        bool compatible = std::all_of(std::next(members.begin()), members.end(),
            [&instances, owner_index](size_t index) {
                return raft_instances_compatible(instances[owner_index], instances[index]);
            });
        ExPolygons support_regions;
        ExPolygons interface_regions;
        if (compatible) {
            for (size_t index : members) {
                RaftRoleRegions local_regions;
                if (!get_raft_role_regions(instances[index], local_regions)) {
                    compatible = false;
                    break;
                }
                translate(local_regions.support, instances[index].shift);
                translate(local_regions.interface, instances[index].shift);
                expolygons_append(support_regions, std::move(local_regions.support));
                expolygons_append(interface_regions, std::move(local_regions.interface));
            }
        }

        if (compatible) {
            support_regions = union_ex(support_regions);
            interface_regions = union_ex(interface_regions);
            if (!support_regions.empty() && !interface_regions.empty())
                interface_regions = diff_ex(interface_regions, support_regions);
            translate(support_regions, -instances[owner_index].shift);
            translate(interface_regions, -instances[owner_index].shift);
            result[owner_index] = generate_merged_raft_paths(
                instances[owner_index], std::move(support_regions), std::move(interface_regions));
            compatible = result[owner_index] != nullptr;
        }

        if (compatible) {
            for (size_t index : members)
                if (index != owner_index) {
                    result[index] = std::make_unique<ExtrusionEntityCollection>();
                    result[index]->no_sort = instances[index].paths != nullptr && instances[index].paths->no_sort;
                }
            continue;
        }

        std::vector<RaftPathInstance> fallback_instances;
        fallback_instances.reserve(members.size());
        for (size_t index : members)
            fallback_instances.emplace_back(instances[index]);
        auto fallback = clip_overlapping_raft_paths(fallback_instances);
        for (size_t member_index = 0; member_index < members.size(); ++member_index)
            result[members[member_index]] = std::move(fallback[member_index]);
    }

    return result;
}

// Support layers, partially processed.
struct SupportGeneratorLayerExtruded
{
    SupportGeneratorLayerExtruded& operator=(SupportGeneratorLayerExtruded &&rhs) {
        this->layer = rhs.layer;
        this->extrusions = std::move(rhs.extrusions);
        m_polygons_to_extrude = std::move(rhs.m_polygons_to_extrude);
        rhs.layer = nullptr;
        return *this;
    }

    bool empty() const {
        return layer == nullptr || layer->polygons.empty();
    }

    void set_polygons_to_extrude(Polygons &&polygons) {
        if (m_polygons_to_extrude == nullptr)
            m_polygons_to_extrude = std::make_unique<Polygons>(std::move(polygons));
        else
            *m_polygons_to_extrude = std::move(polygons);
    }
    Polygons& polygons_to_extrude() { return (m_polygons_to_extrude == nullptr) ? layer->polygons : *m_polygons_to_extrude; }
    const Polygons& polygons_to_extrude() const { return (m_polygons_to_extrude == nullptr) ? layer->polygons : *m_polygons_to_extrude; }

    bool could_merge(const SupportGeneratorLayerExtruded &other) const {
        return ! this->empty() && ! other.empty() &&
            std::abs(this->layer->height - other.layer->height) < EPSILON &&
            this->layer->bridging == other.layer->bridging;
    }

    // Merge regions, perform boolean union over the merged polygons.
    void merge(SupportGeneratorLayerExtruded &&other) {
        assert(this->could_merge(other));
        // 1) Merge the rest polygons to extrude, if there are any.
        if (other.m_polygons_to_extrude != nullptr) {
            if (m_polygons_to_extrude == nullptr) {
                // This layer has no extrusions generated yet, if it has no m_polygons_to_extrude (its area to extrude was not reduced yet).
                assert(this->extrusions.empty());
                m_polygons_to_extrude = std::make_unique<Polygons>(this->layer->polygons);
            }
            Slic3r::polygons_append(*m_polygons_to_extrude, std::move(*other.m_polygons_to_extrude));
            *m_polygons_to_extrude = union_safety_offset(*m_polygons_to_extrude);
            other.m_polygons_to_extrude.reset();
        } else if (m_polygons_to_extrude != nullptr) {
            assert(other.m_polygons_to_extrude == nullptr);
            // The other layer has no extrusions generated yet, if it has no m_polygons_to_extrude (its area to extrude was not reduced yet).
            assert(other.extrusions.empty());
            Slic3r::polygons_append(*m_polygons_to_extrude, other.layer->polygons);
            *m_polygons_to_extrude = union_safety_offset(*m_polygons_to_extrude);
        }
        // 2) Merge the extrusions.
        this->extrusions.insert(this->extrusions.end(), other.extrusions.begin(), other.extrusions.end());
        other.extrusions.clear();
        // 3) Merge the infill polygons.
        Slic3r::polygons_append(this->layer->polygons, std::move(other.layer->polygons));
        this->layer->polygons = union_safety_offset(this->layer->polygons);
        other.layer->polygons.clear();
    }

    void polygons_append(Polygons &dst) const {
        if (layer != NULL && ! layer->polygons.empty())
            Slic3r::polygons_append(dst, layer->polygons);
    }

    // The source layer. It carries the height and extrusion type (bridging / non bridging, extrusion height).
    SupportGeneratorLayer  *layer { nullptr };
    // Collect extrusions. They will be exported sorted by the bottom height.
    ExtrusionEntitiesPtr                  extrusions;

private:
    // In case the extrusions are non-empty, m_polygons_to_extrude may contain the rest areas yet to be filled by additional support.
    // This is useful mainly for the loop interfaces, which are generated before the zig-zag infills.
    std::unique_ptr<Polygons>             m_polygons_to_extrude;
};

typedef std::vector<SupportGeneratorLayerExtruded*> SupportGeneratorLayerExtrudedPtrs;

struct LoopInterfaceProcessor
{
    LoopInterfaceProcessor(coordf_t circle_r) :
        n_contact_loops(0),
        circle_radius(circle_r),
        circle_distance(circle_r * 3.)
    {
        // Shape of the top contact area.
        circle.points.reserve(6);
        for (size_t i = 0; i < 6; ++ i) {
            double angle = double(i) * M_PI / 3.;
            circle.points.push_back(Point(circle_radius * cos(angle), circle_radius * sin(angle)));
        }
    }

    // Generate loop contacts at the top_contact_layer,
    // trim the top_contact_layer->polygons with the areas covered by the loops.
    void generate(SupportGeneratorLayerExtruded &top_contact_layer, const Flow &interface_flow_src) const;

    int         n_contact_loops;
    coordf_t    circle_radius;
    coordf_t    circle_distance;
    Polygon     circle;
};

void LoopInterfaceProcessor::generate(SupportGeneratorLayerExtruded &top_contact_layer, const Flow &interface_flow_src) const
{
    if (n_contact_loops == 0 || top_contact_layer.empty())
        return;

    Flow flow = interface_flow_src.with_height(top_contact_layer.layer->height);

    Polygons overhang_polygons;
    if (top_contact_layer.layer->overhang_polygons != nullptr)
        overhang_polygons = std::move(*top_contact_layer.layer->overhang_polygons);

    // Generate the outermost loop.
    // Find centerline of the external loop (or any other kind of extrusions should the loop be skipped)
    ExPolygons top_contact_expolygons = offset_ex(union_ex(top_contact_layer.layer->polygons), - 0.5f * flow.scaled_width());

    // Grid size and bit shifts for quick and exact to/from grid coordinates manipulation.
    coord_t circle_grid_resolution = 1;
    coord_t circle_grid_powerof2 = 0;
    {
        // epsilon to account for rounding errors
        coord_t circle_grid_resolution_non_powerof2 = coord_t(2. * circle_distance + 3.);
        while (circle_grid_resolution < circle_grid_resolution_non_powerof2) {
            circle_grid_resolution <<= 1;
            ++ circle_grid_powerof2;
        }
    }

    struct PointAccessor {
        const Point* operator()(const Point &pt) const { return &pt; }
    };
    typedef ClosestPointInRadiusLookup<Point, PointAccessor> ClosestPointLookupType;

    Polygons loops0;
    {
        // find centerline of the external loop of the contours
        // Only consider the loops facing the overhang.
        Polygons external_loops;
        // Holes in the external loops.
        Polygons circles;
        Polygons overhang_with_margin = offset(union_ex(overhang_polygons), 0.5f * flow.scaled_width());
        for (ExPolygons::iterator it_contact_expoly = top_contact_expolygons.begin(); it_contact_expoly != top_contact_expolygons.end(); ++ it_contact_expoly) {
            // Store the circle centers placed for an expolygon into a regular grid, hashed by the circle centers.
            ClosestPointLookupType circle_centers_lookup(coord_t(circle_distance - SCALED_EPSILON));
            Points circle_centers;
            Point  center_last;
            // For each contour of the expolygon, start with the outer contour, continue with the holes.
            for (size_t i_contour = 0; i_contour <= it_contact_expoly->holes.size(); ++ i_contour) {
                Polygon     &contour = (i_contour == 0) ? it_contact_expoly->contour : it_contact_expoly->holes[i_contour - 1];
                const Point *seg_current_pt = nullptr;
                coordf_t     seg_current_t  = 0.;
                if (! intersection_pl(contour.split_at_first_point(), overhang_with_margin).empty()) {
                    // The contour is below the overhang at least to some extent.
                    //FIXME ideally one would place the circles below the overhang only.
                    // Walk around the contour and place circles so their centers are not closer than circle_distance from each other.
                    if (circle_centers.empty()) {
                        // Place the first circle.
                        seg_current_pt = &contour.points.front();
                        seg_current_t  = 0.;
                        center_last    = *seg_current_pt;
                        circle_centers_lookup.insert(center_last);
                        circle_centers.push_back(center_last);
                    }
                    for (Points::const_iterator it = contour.points.begin() + 1; it != contour.points.end(); ++it) {
                        // Is it possible to place a circle on this segment? Is it not too close to any of the circles already placed on this contour?
                        const Point &p1 = *(it-1);
                        const Point &p2 = *it;
                        // Intersection of a ray (p1, p2) with a circle placed at center_last, with radius of circle_distance.
                        const Vec2d v_seg(coordf_t(p2(0)) - coordf_t(p1(0)), coordf_t(p2(1)) - coordf_t(p1(1)));
                        const Vec2d v_cntr(coordf_t(p1(0) - center_last(0)), coordf_t(p1(1) - center_last(1)));
                        coordf_t a = v_seg.squaredNorm();
                        coordf_t b = 2. * v_seg.dot(v_cntr);
                        coordf_t c = v_cntr.squaredNorm() - circle_distance * circle_distance;
                        coordf_t disc = b * b - 4. * a * c;
                        if (disc > 0.) {
                            // The circle intersects a ray. Avoid the parts of the segment inside the circle.
                            coordf_t t1 = (-b - sqrt(disc)) / (2. * a);
                            coordf_t t2 = (-b + sqrt(disc)) / (2. * a);
                            coordf_t t0 = (seg_current_pt == &p1) ? seg_current_t : 0.;
                            // Take the lowest t in <t0, 1.>, excluding <t1, t2>.
                            coordf_t t;
                            if (t0 <= t1)
                                t = t0;
                            else if (t2 <= 1.)
                                t = t2;
                            else {
                                // Try the following segment.
                                seg_current_pt = nullptr;
                                continue;
                            }
                            seg_current_pt = &p1;
                            seg_current_t  = t;
                            center_last    = Point(p1(0) + coord_t(v_seg(0) * t), p1(1) + coord_t(v_seg(1) * t));
                            // It has been verified that the new point is far enough from center_last.
                            // Ensure, that it is far enough from all the centers.
                            std::pair<const Point*, coordf_t> circle_closest = circle_centers_lookup.find(center_last);
                            if (circle_closest.first != nullptr) {
                                -- it;
                                continue;
                            }
                        } else {
                            // All of the segment is outside the circle. Take the first point.
                            seg_current_pt = &p1;
                            seg_current_t  = 0.;
                            center_last    = p1;
                        }
                        // Place the first circle.
                        circle_centers_lookup.insert(center_last);
                        circle_centers.push_back(center_last);
                    }
                    external_loops.push_back(std::move(contour));
                    for (const Point &center : circle_centers) {
                        circles.push_back(circle);
                        circles.back().translate(center);
                    }
                }
            }
        }
        // Apply a pattern to the external loops.
        loops0 = diff(external_loops, circles);
    }

    Polylines loop_lines;
    {
        // make more loops
        Polygons loop_polygons = loops0;
        for (int i = 1; i < n_contact_loops; ++ i)
            polygons_append(loop_polygons,
                opening(
                    loops0,
                    i * flow.scaled_spacing() + 0.5f * flow.scaled_spacing(),
                    0.5f * flow.scaled_spacing()));
        // Clip such loops to the side oriented towards the object.
        // Collect split points, so they will be recognized after the clipping.
        // At the split points the clipped pieces will be stitched back together.
        loop_lines.reserve(loop_polygons.size());
        std::unordered_map<Point, int, PointHash> map_split_points;
        for (Polygons::const_iterator it = loop_polygons.begin(); it != loop_polygons.end(); ++ it) {
            assert(map_split_points.find(it->first_point()) == map_split_points.end());
            map_split_points[it->first_point()] = -1;
            loop_lines.push_back(it->split_at_first_point());
        }
        loop_lines = intersection_pl(loop_lines, expand(overhang_polygons, scale_(SUPPORT_MATERIAL_MARGIN)));
        // Because a closed loop has been split to a line, loop_lines may contain continuous segments split to 2 pieces.
        // Try to connect them.
        for (int i_line = 0; i_line < int(loop_lines.size()); ++ i_line) {
            Polyline &polyline = loop_lines[i_line];
            auto it = map_split_points.find(polyline.first_point());
            if (it != map_split_points.end()) {
                // This is a stitching point.
                // If this assert triggers, multiple source polygons likely intersected at this point.
                assert(it->second != -2);
                if (it->second < 0) {
                    // First occurence.
                    it->second = i_line;
                } else {
                    // Second occurence. Join the lines.
                    Polyline &polyline_1st = loop_lines[it->second];
                    assert(polyline_1st.first_point() == it->first || polyline_1st.last_point() == it->first);
                    if (polyline_1st.first_point() == it->first)
                        polyline_1st.reverse();
                    polyline_1st.append(std::move(polyline));
                    it->second = -2;
                }
                continue;
            }
            it = map_split_points.find(polyline.last_point());
            if (it != map_split_points.end()) {
                // This is a stitching point.
                // If this assert triggers, multiple source polygons likely intersected at this point.
                assert(it->second != -2);
                if (it->second < 0) {
                    // First occurence.
                    it->second = i_line;
                } else {
                    // Second occurence. Join the lines.
                    Polyline &polyline_1st = loop_lines[it->second];
                    assert(polyline_1st.first_point() == it->first || polyline_1st.last_point() == it->first);
                    if (polyline_1st.first_point() == it->first)
                        polyline_1st.reverse();
                    polyline.reverse();
                    polyline_1st.append(std::move(polyline));
                    it->second = -2;
                }
            }
        }
        // Remove empty lines.
        remove_degenerate(loop_lines);
    }

    // add the contact infill area to the interface area
    // note that growing loops by $circle_radius ensures no tiny
    // extrusions are left inside the circles; however it creates
    // a very large gap between loops and contact_infill_polygons, so maybe another
    // solution should be found to achieve both goals
    // Store the trimmed polygons into a separate polygon set, so the original infill area remains intact for
    // "modulate by layer thickness".
    top_contact_layer.set_polygons_to_extrude(diff(top_contact_layer.layer->polygons, offset(loop_lines, float(circle_radius * 1.1))));

    // Transform loops into ExtrusionPath objects.
    extrusion_entities_append_paths(
        top_contact_layer.extrusions,
        std::move(loop_lines),
        ExtrusionRole::erSupportMaterialInterface, flow.mm3_per_mm(), flow.width(), flow.height());
}

#ifdef SLIC3R_DEBUG
static std::string dbg_index_to_color(int idx)
{
    if (idx < 0)
        return "yellow";
    idx = idx % 3;
    switch (idx) {
        case 0: return "red";
        case 1: return "green";
        default: return "blue";
    }
}
#endif /* SLIC3R_DEBUG */

// When extruding a bottom interface layer over an object, the bottom interface layer is extruded in a thin air, therefore
// it is being extruded with a bridging flow to not shrink excessively (the die swell effect).
// Tiny extrusions are better avoided and it is always better to anchor the thread to an existing support structure if possible.
// Therefore the bottom interface spots are expanded a bit. The expanded regions may overlap with another bottom interface layers,
// leading to over extrusion, where they overlap. The over extrusion is better avoided as it often makes the interface layers
// to stick too firmly to the object.
//
// Modulate thickness (increase bottom_z) of extrusions_in_out generated for this_layer
// if they overlap with overlapping_layers, whose print_z is above this_layer.bottom_z() and below this_layer.print_z.
static void modulate_extrusion_by_overlapping_layers(
    // Extrusions generated for this_layer.
    ExtrusionEntitiesPtr                               &extrusions_in_out,
    const SupportGeneratorLayer          &this_layer,
    // Multiple layers overlapping with this_layer, sorted bottom up.
    const SupportGeneratorLayersPtr      &overlapping_layers)
{
    size_t n_overlapping_layers = overlapping_layers.size();
    if (n_overlapping_layers == 0 || extrusions_in_out.empty())
        // The extrusions do not overlap with any other extrusion.
        return;

    // Hollow organic walls are ordered collections. Keep that ordering and
    // recurse into their paths before the flow-specific fragmentation below;
    // treating a collection as an ExtrusionPath dereferences a null cast.
    if (std::any_of(extrusions_in_out.begin(), extrusions_in_out.end(),
            [](const ExtrusionEntity *entity) { return dynamic_cast<const ExtrusionEntityCollection *>(entity) != nullptr; })) {
        ExtrusionEntitiesPtr source = std::move(extrusions_in_out);
        extrusions_in_out.clear();
        for (size_t first = 0; first < source.size();) {
            if (auto *collection = dynamic_cast<ExtrusionEntityCollection *>(source[first])) {
                modulate_extrusion_by_overlapping_layers(collection->entities, this_layer, overlapping_layers);
                if (collection->empty())
                    delete collection;
                else
                    extrusions_in_out.push_back(collection);
                ++first;
            } else {
                size_t last = first + 1;
                while (last < source.size() && dynamic_cast<ExtrusionEntityCollection *>(source[last]) == nullptr)
                    ++last;
                ExtrusionEntitiesPtr run(source.begin() + first, source.begin() + last);
                modulate_extrusion_by_overlapping_layers(run, this_layer, overlapping_layers);
                append(extrusions_in_out, std::move(run));
                first = last;
            }
        }
        return;
    }

    // Walls use the nominal flow, but narrow contact gap fill has variable
    // widths. Modulating the whole vector with its first path's flow would
    // widen those thin paths again. Process consecutive homogeneous runs while
    // retaining their original order and every segment's extrusion parameters.
    auto same_flow = [](const ExtrusionEntity *lhs, const ExtrusionEntity *rhs) {
        const auto *a = dynamic_cast<const ExtrusionPath*>(lhs);
        const auto *b = dynamic_cast<const ExtrusionPath*>(rhs);
        assert(a != nullptr && b != nullptr);
        return a->role() == b->role() && a->width == b->width &&
               a->height == b->height && a->mm3_per_mm == b->mm3_per_mm && a->can_reverse() == b->can_reverse();
    };
    if (std::any_of(extrusions_in_out.begin() + 1, extrusions_in_out.end(),
                    [&](const ExtrusionEntity *entity) { return !same_flow(extrusions_in_out.front(), entity); })) {
        ExtrusionEntitiesPtr source = std::move(extrusions_in_out);
        extrusions_in_out.clear();
        for (size_t first = 0; first < source.size();) {
            size_t last = first + 1;
            while (last < source.size() && same_flow(source[first], source[last]))
                ++last;
            ExtrusionEntitiesPtr run(source.begin() + first, source.begin() + last);
            modulate_extrusion_by_overlapping_layers(run, this_layer, overlapping_layers);
            append(extrusions_in_out, std::move(run));
            first = last;
        }
        return;
    }

    // Get the initial extrusion parameters.
    ExtrusionPath *extrusion_path_template = dynamic_cast<ExtrusionPath*>(extrusions_in_out.front());
    assert(extrusion_path_template != nullptr);
    ExtrusionRole extrusion_role  = extrusion_path_template->role();
    float         extrusion_width = extrusion_path_template->width;
    const bool    reversible = extrusion_path_template->can_reverse();

    struct ExtrusionPathFragment
    {
        ExtrusionPathFragment() : mm3_per_mm(-1), width(-1), height(-1) {};
        ExtrusionPathFragment(double mm3_per_mm, float width, float height) : mm3_per_mm(mm3_per_mm), width(width), height(height) {};

        Polylines       polylines;
        double          mm3_per_mm;
        float           width;
        float           height;
    };

    // Split the extrusions by the overlapping layers, reduce their extrusion rate.
    // The last path_fragment is from this_layer.
    std::vector<ExtrusionPathFragment> path_fragments(
        n_overlapping_layers + 1,
        ExtrusionPathFragment(extrusion_path_template->mm3_per_mm, extrusion_path_template->width, extrusion_path_template->height));
    // Don't use it, it will be released.
    extrusion_path_template = nullptr;

#ifdef SLIC3R_DEBUG
    static int iRun = 0;
    ++ iRun;
    BoundingBox bbox;
    for (size_t i_overlapping_layer = 0; i_overlapping_layer < n_overlapping_layers; ++ i_overlapping_layer) {
        const SupportGeneratorLayer &overlapping_layer = *overlapping_layers[i_overlapping_layer];
        bbox.merge(get_extents(overlapping_layer.polygons));
    }
    for (ExtrusionEntitiesPtr::const_iterator it = extrusions_in_out.begin(); it != extrusions_in_out.end(); ++ it) {
        ExtrusionPath *path = dynamic_cast<ExtrusionPath*>(*it);
        assert(path != nullptr);
        bbox.merge(get_extents(path->polyline));
    }
    SVG svg(debug_out_path("support-fragments-%d-%lf.svg", iRun, this_layer.print_z).c_str(), bbox);
    const float transparency = 0.5f;
    // Filled polygons for the overlapping regions.
    svg.draw(union_ex(this_layer.polygons), dbg_index_to_color(-1), transparency);
    for (size_t i_overlapping_layer = 0; i_overlapping_layer < n_overlapping_layers; ++ i_overlapping_layer) {
        const SupportGeneratorLayer &overlapping_layer = *overlapping_layers[i_overlapping_layer];
        svg.draw(union_ex(overlapping_layer.polygons), dbg_index_to_color(int(i_overlapping_layer)), transparency);
    }
    // Contours of the overlapping regions.
    svg.draw(to_polylines(this_layer.polygons), dbg_index_to_color(-1), scale_(0.2));
    for (size_t i_overlapping_layer = 0; i_overlapping_layer < n_overlapping_layers; ++ i_overlapping_layer) {
        const SupportGeneratorLayer &overlapping_layer = *overlapping_layers[i_overlapping_layer];
        svg.draw(to_polylines(overlapping_layer.polygons), dbg_index_to_color(int(i_overlapping_layer)), scale_(0.1));
    }
    // Fill extrusion, the source.
    for (ExtrusionEntitiesPtr::const_iterator it = extrusions_in_out.begin(); it != extrusions_in_out.end(); ++ it) {
        ExtrusionPath *path = dynamic_cast<ExtrusionPath*>(*it);
        std::string color_name;
        switch ((it - extrusions_in_out.begin()) % 9) {
            case 0: color_name = "magenta"; break;
            case 1: color_name = "deepskyblue"; break;
            case 2: color_name = "coral"; break;
            case 3: color_name = "goldenrod"; break;
            case 4: color_name = "orange"; break;
            case 5: color_name = "olivedrab"; break;
            case 6: color_name = "blueviolet"; break;
            case 7: color_name = "brown"; break;
            default: color_name = "orchid"; break;
        }
        svg.draw(path->polyline, color_name, scale_(0.2));
    }
#endif /* SLIC3R_DEBUG */

    // End points of the original paths.
    std::vector<std::pair<Point, Point>> path_ends;
    // Collect the paths of this_layer.
    {
        Polylines &polylines = path_fragments.back().polylines;
        for (ExtrusionEntity *ee : extrusions_in_out) {
            ExtrusionPath *path = dynamic_cast<ExtrusionPath*>(ee);
            assert(path != nullptr);
            polylines.emplace_back(path->polyline.to_polyline());
            path_ends.emplace_back(std::pair<Point, Point>(polylines.back().points.front(), polylines.back().points.back()));
            delete path;
        }
    }
    // Destroy the original extrusion paths, their polylines were moved to path_fragments already.
    // This will be the destination for the new paths.
    extrusions_in_out.clear();

    // Fragment the path segments by overlapping layers. The overlapping layers are sorted by an increasing print_z.
    // Trim by the highest overlapping layer first.
    for (int i_overlapping_layer = int(n_overlapping_layers) - 1; i_overlapping_layer >= 0; -- i_overlapping_layer) {
        const SupportGeneratorLayer &overlapping_layer = *overlapping_layers[i_overlapping_layer];
        ExtrusionPathFragment &frag = path_fragments[i_overlapping_layer];
        Polygons polygons_trimming = offset(union_ex(overlapping_layer.polygons), float(scale_(0.5*extrusion_width)));
        frag.polylines = intersection_pl(path_fragments.back().polylines, polygons_trimming);
        path_fragments.back().polylines = diff_pl(path_fragments.back().polylines, polygons_trimming);
        // Adjust the extrusion parameters for a reduced layer height and a non-bridging flow (nozzle_dmr = -1, does not matter).
        assert(this_layer.print_z > overlapping_layer.print_z);
        frag.height = float(this_layer.print_z - overlapping_layer.print_z);
        frag.mm3_per_mm = Flow(frag.width, frag.height, -1.f).mm3_per_mm();
#ifdef SLIC3R_DEBUG
        svg.draw(frag.polylines, dbg_index_to_color(i_overlapping_layer), scale_(0.1));
#endif /* SLIC3R_DEBUG */
    }

#ifdef SLIC3R_DEBUG
    svg.draw(path_fragments.back().polylines, dbg_index_to_color(-1), scale_(0.1));
    svg.Close();
#endif /* SLIC3R_DEBUG */

    // Now chain the split segments using hashing and a nearly exact match, maintaining the order of segments.
    // Create a single ExtrusionPath or ExtrusionEntityCollection per source ExtrusionPath.
    // Map of fragment start/end points to a pair of <i_overlapping_layer, i_polyline_in_layer>
    // Because a non-exact matching is used for the end points, a multi-map is used.
    // As the clipper library may reverse the order of some clipped paths, store both ends into the map.
    struct ExtrusionPathFragmentEnd
    {
        ExtrusionPathFragmentEnd(size_t alayer_idx, size_t apolyline_idx, bool ais_start) :
            layer_idx(alayer_idx), polyline_idx(apolyline_idx), is_start(ais_start) {}
        size_t layer_idx;
        size_t polyline_idx;
        bool   is_start;
    };
    class ExtrusionPathFragmentEndPointAccessor {
    public:
        ExtrusionPathFragmentEndPointAccessor(const std::vector<ExtrusionPathFragment> &path_fragments) : m_path_fragments(path_fragments) {}
        // Return an end point of a fragment, or nullptr if the fragment has been consumed already.
        const Point* operator()(const ExtrusionPathFragmentEnd &fragment_end) const {
            const Polyline &polyline = m_path_fragments[fragment_end.layer_idx].polylines[fragment_end.polyline_idx];
            return polyline.points.empty() ? nullptr :
                (fragment_end.is_start ? &polyline.points.front() : &polyline.points.back());
        }
    private:
        const std::vector<ExtrusionPathFragment> &m_path_fragments;
    };
    const coord_t search_radius = 7;
    ClosestPointInRadiusLookup<ExtrusionPathFragmentEnd, ExtrusionPathFragmentEndPointAccessor> map_fragment_starts(
        search_radius, ExtrusionPathFragmentEndPointAccessor(path_fragments));
    for (size_t i_overlapping_layer = 0; i_overlapping_layer <= n_overlapping_layers; ++ i_overlapping_layer) {
        const Polylines &polylines = path_fragments[i_overlapping_layer].polylines;
        for (size_t i_polyline = 0; i_polyline < polylines.size(); ++ i_polyline) {
            // Map a starting point of a polyline to a pair of <layer, polyline>
            if (polylines[i_polyline].points.size() >= 2) {
                map_fragment_starts.insert(ExtrusionPathFragmentEnd(i_overlapping_layer, i_polyline, true));
                map_fragment_starts.insert(ExtrusionPathFragmentEnd(i_overlapping_layer, i_polyline, false));
            }
        }
    }

    // For each source path:
    for (size_t i_path = 0; i_path < path_ends.size(); ++ i_path) {
        const Point &pt_start = path_ends[i_path].first;
        const Point &pt_end   = path_ends[i_path].second;
        Point pt_current = pt_start;
        // Find a chain of fragments with the original / reduced print height.
        ExtrusionMultiPath multipath;
        for (;;) {
            // Find a closest end point to pt_current.
            std::pair<const ExtrusionPathFragmentEnd*, coordf_t> end_and_dist2 = map_fragment_starts.find(pt_current);
            // There may be a bug in Clipper flipping the order of two last points in a fragment?
            // assert(end_and_dist2.first != nullptr);
            assert(end_and_dist2.first == nullptr || end_and_dist2.second < search_radius * search_radius);
            if (end_and_dist2.first == nullptr) {
                // New fragment connecting to pt_current was not found.
                // Verify that the last point found is close to the original end point of the unfragmented path.
                //const double d2 = (pt_end - pt_current).cast<double>.squaredNorm();
                //assert(d2 < coordf_t(search_radius * search_radius));
                // End of the path.
                break;
            }
            const ExtrusionPathFragmentEnd &fragment_end_min = *end_and_dist2.first;
            // Fragment to consume.
            ExtrusionPathFragment &frag = path_fragments[fragment_end_min.layer_idx];
            Polyline              &frag_polyline = frag.polylines[fragment_end_min.polyline_idx];
            // Path to append the fragment to.
            ExtrusionPath         *path = multipath.paths.empty() ? nullptr : &multipath.paths.back();
            if (path != nullptr) {
                // Verify whether the path is compatible with the current fragment.
                // Base and interface pieces can share a predecessor Z. Keep
                // adjacent fragments together when their resulting flow matches.
                if (path->height != frag.height || path->mm3_per_mm != frag.mm3_per_mm) {
                    path = nullptr;
                }
                // Merging with the previous path. This can only happen if the current layer was reduced by a base layer, which was split into a base and interface layer.
            }
            if (path == nullptr) {
                // Allocate a new path.
                multipath.paths.push_back(ExtrusionPath(extrusion_role, frag.mm3_per_mm, frag.width, frag.height));
                path = &multipath.paths.back();
                if (!reversible)
                    path->set_reverse();
            }
            // The Clipper library may flip the order of the clipped polylines arbitrarily.
            // Reverse the source polyline, if connecting to the end.
            if (! fragment_end_min.is_start)
                frag_polyline.reverse();
            // Enforce exact overlap of the end points of successive fragments.
            assert(frag_polyline.points.front() == pt_current);
            frag_polyline.points.front() = pt_current;
            // Don't repeat the first point.
            if (! path->polyline.points.empty())
                path->polyline.points.pop_back();
            // Consume the fragment's polyline, remove it from the input fragments, so it will be ignored the next time.
            path->polyline.append(Polyline3(std::move(frag_polyline)));
            frag_polyline.points.clear();
            const Point3 &pt_back3 = path->polyline.points.back();
            pt_current = Point(pt_back3.x(), pt_back3.y());
            if (pt_current == pt_end) {
                // End of the path.
                break;
            }
        }
        if (!multipath.paths.empty()) {
            if (!reversible)
                multipath.set_reverse();
            if (multipath.paths.size() == 1) {
                // This path was not fragmented.
                extrusions_in_out.push_back(new ExtrusionPath(std::move(multipath.paths.front())));
            } else {
                // This path was fragmented. Copy the collection as a whole object, so the order inside the collection will not be changed
                // during the chaining of extrusions_in_out.
                extrusions_in_out.push_back(new ExtrusionMultiPath(std::move(multipath)));
            }
        }
    }
    // If there are any non-consumed fragments, add them separately.
    //FIXME this shall not happen, if the Clipper works as expected and all paths split to fragments could be re-connected.
    for (auto it_fragment = path_fragments.begin(); it_fragment != path_fragments.end(); ++ it_fragment) {
        const size_t first = extrusions_in_out.size();
        extrusion_entities_append_paths(extrusions_in_out, std::move(it_fragment->polylines), extrusion_role,
            it_fragment->mm3_per_mm, it_fragment->width, it_fragment->height, reversible);
        if (!reversible)
            for (size_t index = first; index < extrusions_in_out.size(); ++index)
                extrusions_in_out[index]->set_reverse();
    }
}

// Support layer that is covered by some form of dense interface.
static constexpr const std::initializer_list<SupporLayerType> support_types_interface{
    SupporLayerType::RaftInterface, SupporLayerType::BottomContact, SupporLayerType::BottomInterface, SupporLayerType::TopContact, SupporLayerType::TopInterface
};

SupportGeneratorLayersPtr generate_support_layers(
    PrintObject                         &object,
    const SupportGeneratorLayersPtr     &raft_layers,
    const SupportGeneratorLayersPtr     &bottom_contacts,
    const SupportGeneratorLayersPtr     &top_contacts,
    const SupportGeneratorLayersPtr     &intermediate_layers,
    const SupportGeneratorLayersPtr     &interface_layers,
    const SupportGeneratorLayersPtr     &base_interface_layers)
{
    // Install support layers into the object.
    // A support layer installed on a PrintObject has a unique print_z.
    SupportGeneratorLayersPtr layers_sorted;
    layers_sorted.reserve(raft_layers.size() + bottom_contacts.size() + top_contacts.size() + intermediate_layers.size() + interface_layers.size() + base_interface_layers.size());
    append(layers_sorted, raft_layers);
    append(layers_sorted, bottom_contacts);
    append(layers_sorted, top_contacts);
    append(layers_sorted, intermediate_layers);
    append(layers_sorted, interface_layers);
    append(layers_sorted, base_interface_layers);
    // remove dupliated layers
    std::sort(layers_sorted.begin(), layers_sorted.end());
    layers_sorted.erase(std::unique(layers_sorted.begin(), layers_sorted.end()), layers_sorted.end());

    // Sort the layers lexicographically by a raising print_z and a decreasing height.
    std::sort(layers_sorted.begin(), layers_sorted.end(), [](auto *l1, auto *l2) { return *l1 < *l2; });
    int layer_id = 0;
    int layer_id_interface = 0;
    assert(object.support_layers().empty());
    for (size_t i = 0; i < layers_sorted.size();) {
        // Find the last layer with roughly the same print_z, find the minimum layer height of all.
        // Due to the floating point inaccuracies, the print_z may not be the same even if in theory they should.
        size_t j = i + 1;
        coordf_t zmax = layers_sorted[i]->print_z + EPSILON;
        for (; j < layers_sorted.size() && layers_sorted[j]->print_z <= zmax; ++j) ;
        // Assign an average print_z to the set of layers with nearly equal print_z.
        coordf_t zavg = 0.5 * (layers_sorted[i]->print_z + layers_sorted[j - 1]->print_z);
        coordf_t height_min = layers_sorted[i]->height;
        bool     empty = true;
        // For snug supports, layers where the direction of the support interface shall change are accounted for.
        size_t   num_interfaces = 0;
        size_t   num_top_contacts = 0;
        double   top_contact_bottom_z = 0;
        for (size_t u = i; u < j; ++u) {
            SupportGeneratorLayer &layer = *layers_sorted[u];
            if (! layer.polygons.empty()) {
                empty             = false;
                const bool is_base_interface = std::find(base_interface_layers.begin(), base_interface_layers.end(), &layer) != base_interface_layers.end();
                num_interfaces += one_of(layer.layer_type, support_types_interface) || is_base_interface;
                if (layer.layer_type == SupporLayerType::TopContact) {
                    ++ num_top_contacts;
                    assert(num_top_contacts <= 1);
                    // All top contact layers sharing this print_z shall also share bottom_z.
                    //assert(num_top_contacts == 1 || (top_contact_bottom_z - layer.bottom_z) < EPSILON);
                    top_contact_bottom_z = layer.bottom_z;
                }
            }
            layer.print_z = zavg;
            height_min = std::min(height_min, layer.height);
        }
        if (! empty) {
            // Here the upper_layer and lower_layer pointers are left to null at the support layers,
            // as they are never used. These pointers are candidates for removal.
            bool   this_layer_contacts_only = num_top_contacts > 0 && num_top_contacts == num_interfaces;
            size_t this_layer_id_interface  = layer_id_interface;
            if (this_layer_contacts_only) {
                // Find a supporting layer for its interface ID.
                for (auto it = object.support_layers().rbegin(); it != object.support_layers().rend(); ++ it)
                    if (const SupportLayer &other_layer = **it; std::abs(other_layer.print_z - top_contact_bottom_z) < EPSILON) {
                        // other_layer supports this top contact layer. Assign a different support interface direction to this layer
                        // from the layer that supports it.
                        this_layer_id_interface = other_layer.interface_id() + 1;
                    }
            }
            object.add_support_layer(layer_id ++, this_layer_id_interface, height_min, zavg);
            if (num_interfaces && ! this_layer_contacts_only)
                ++ layer_id_interface;
        }
        i = j;
    }
    return layers_sorted;
}

// Evaluate the physical bead after overlapping support planes have reduced its
// height. Filtering at the nominal height would also erase valid thin contacts.
// Split mixed-flow entities into their surviving continuous runs; never create
// an extrusion bridge across an unprintable part.
static void remove_unprintable_interface_paths(ExtrusionEntitiesPtr &entities)
{
    auto invalid = [](const ExtrusionPath &path) {
        return path.role() == erSupportMaterialInterface && !path.z_contoured &&
            path.height > 0.f && path.width + float(EPSILON) < path.height;
    };
    ExtrusionEntitiesPtr result;
    for (ExtrusionEntity *entity : entities) {
        if (auto *collection = dynamic_cast<ExtrusionEntityCollection *>(entity)) {
            remove_unprintable_interface_paths(collection->entities);
            if (collection->empty()) delete collection;
            else result.push_back(collection);
            continue;
        }
        bool needs_filter = false;
        for_each_extrusion_path(*entity, [&](const ExtrusionPath &path) { needs_filter |= invalid(path); });
        if (!needs_filter) { result.push_back(entity); continue; }
        ExtrusionPaths run;
        auto finish_run = [&]() {
            if (run.empty()) return;
            auto *part = new ExtrusionMultiPath;
            part->paths = std::move(run);
            if (!entity->can_reverse()) part->set_reverse();
            result.push_back(part);
            run.clear();
        };
        for_each_extrusion_path(*entity, [&](const ExtrusionPath &path) {
            if (invalid(path)) finish_run();
            else {
                if (!run.empty() && run.back().last_point3() != path.first_point3()) finish_run();
                run.push_back(path);
            }
        });
        finish_run();
        delete entity;
    }
    entities = std::move(result);
}

// Clipping an organic interface projection can leave a compact, isolated
// platform that no later extrusion uses. Only generic interfaces are eligible:
// true model contacts, trunks and substantial platforms keep their geometry.
static void remove_unused_compact_interfaces(
    SupportLayerPtrs &layers,
    const std::vector<std::set<const ExtrusionEntity *>> &generic_interfaces,
    float process_width)
{
    struct Road {
        const ExtrusionEntity *owner;
        const ExtrusionPath *path;
        BoundingBox bounds;
        double bottom;
    };
    std::vector<std::vector<Road>> roads(layers.size());
    for (size_t i = 0; i < layers.size(); ++i)
        for (const ExtrusionEntity *entity : layers[i]->support_fills.entities)
            for_each_extrusion_path(*entity, [&](const ExtrusionPath &path) {
                if (!is_support(path.role()) || path.z_contoured || path.height <= 0.f)
                    return;
                BoundingBox bounds;
                for (const Point3 &point : path.polyline.points)
                    bounds.merge(point.to_point());
                bounds.offset(0.5 * scale_(path.width) + SCALED_EPSILON);
                roads[i].push_back({entity, &path, bounds, layers[i]->print_z - path.height});
            });
    std::multimap<double, const Road *> upper_roads;
    for (const auto &layer : roads)
        for (const Road &road : layer)
            upper_roads.emplace(road.bottom, &road);
    std::set<const ExtrusionEntity *> removed;
    for (size_t index = layers.size(); index-- > 0;) {
        auto &entities = layers[index]->support_fills.entities;
        for (auto it = entities.begin(); it != entities.end();) {
            ExtrusionEntity *entity = *it;
            if (generic_interfaces[index].count(entity) == 0) { ++it; continue; }
            double length = 0.;
            float width = 0.f;
            bool eligible = true;
            for_each_extrusion_path(*entity, [&](const ExtrusionPath &path) {
                eligible = eligible && path.role() == erSupportMaterialInterface && !path.z_contoured &&
                    path.height > 0.f && path.width > 0.f;
                length += path.length();
                width = std::max(width, path.width);
            });
            // A narrow fallback must not redefine what is compact for this
            // process; use the configured interface width for that decision.
            const double bead = scale_(std::max(width, process_width));
            if (!eligible || width <= 0.f || length > 32. * bead) { ++it; continue; }
            Polygons footprint;
            for_each_extrusion_path(*entity, [&](const ExtrusionPath &path) {
                polygons_append(footprint, offset(path.polyline.to_polyline(), float(0.5 * scale_(path.width)),
                    jtRound, double(SCALED_EPSILON), etOpenRound));
            });
            footprint = union_(footprint);
            const BoundingBox bounds = get_extents(footprint);
            // Closed compact islands can have a perimeter much longer than
            // four bead widths. Judge occupied area and span, not only path
            // length, so an unused small ring cannot keep a whole orphan chain.
            if (area(footprint) > 16. * bead * bead ||
                bounds.size().cast<double>().norm() > 8. * bead) { ++it; continue; }
            std::map<float, Polygons> expanded;
            auto touches = [&](const Road &road) {
                if (road.owner == entity || removed.count(road.owner) || !bounds.overlap(road.bounds))
                    return false;
                auto [entry, inserted] = expanded.try_emplace(road.path->width);
                if (inserted)
                    entry->second = offset(footprint, float(0.5 * scale_(road.path->width) + SCALED_EPSILON),
                        jtRound, double(SCALED_EPSILON));
                return !intersection_pl(Polylines{road.path->polyline.to_polyline()}, entry->second).empty();
            };
            bool needed = std::any_of(roads[index].begin(), roads[index].end(), touches);
            const double z = layers[index]->print_z;
            for (auto upper = upper_roads.lower_bound(z - EPSILON);
                 !needed && upper != upper_roads.end() && upper->first <= z + EPSILON; ++upper)
                needed = touches(*upper->second);
            if (needed) { ++it; continue; }
            removed.insert(entity);
            delete entity;
            it = entities.erase(it);
        }
    }
}

// Find a local continuous route in a full-bead center domain. Boundary vertices
// are only needed when projecting the ends of a segment across a concave corner.
bool route_support_path(const Point &from,const Point &to,const ExPolygons &centers,
    const ExPolygons &visible,double reach,Points &output)
{
    if (from==to) return true;
    std::unique_ptr<Clipper2Lib::ReuseableDataContainer64> prepared;
    auto fits=[&](const Point &a,const Point &b) {
        if (prepared) {
            // Same open difference, fill rule, path order and collinearity
            // policy as diff_pl(). Only immutable clip-vertex preparation is
            // reused; every edge still runs the native clipping kernel.
            Clipper2Lib::Clipper64 clipper;
            clipper.PreserveCollinear(false);
            clipper.AddOpenSubject(Clipper2Lib::Paths64{Clipper2Lib::Path64{
                Clipper2Lib::Point64(a.x(), a.y()), Clipper2Lib::Point64(b.x(), b.y())}});
            clipper.AddReuseableData(*prepared);
            Clipper2Lib::Paths64 closed, open;
            clipper.Execute(Clipper2Lib::ClipType::Difference, Clipper2Lib::FillRule::NonZero, closed, open);
            return open.empty();
        }
        return diff_pl(Polylines{Polyline{Points{a,b}}},visible).empty();
    };
    if (fits(from,to)) { output.push_back(to); return true; }
    // A continuous route cannot leave its connected visible material. Group
    // overlapping bounding boxes, conservatively retaining point contacts and
    // native clipping tolerance. Disjoint groups prove that a route is absent;
    // overlapping boxes alone never claim that a route exists.
    std::vector<BoundingBox> bounds;
    for (const ExPolygon &part : visible) {
        BoundingBox box = get_extents(part);
        if (!box.defined) continue;
        box.offset(4 * SCALED_EPSILON);
        bounds.push_back(box);
    }
    if (bounds.size() > 256) {
        BoundingBox all;
        for (const BoundingBox &box : bounds) all.merge(box);
        bounds = {all};
    }
    std::vector<size_t> parent(bounds.size());
    std::iota(parent.begin(), parent.end(), size_t(0));
    auto root = [&](size_t index) {
        while (parent[index] != index) { parent[index] = parent[parent[index]]; index = parent[index]; }
        return index;
    };
    for (size_t i = 0; i < bounds.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (bounds[i].overlap(bounds[j])) parent[root(i)] = root(j);
    std::map<size_t, BoundingBox> grouped;
    for (size_t i = 0; i < bounds.size(); ++i) grouped[root(i)].merge(bounds[i]);
    std::vector<BoundingBox> groups;
    bool possibly_connected = false;
    for (const auto &[index, box] : grouped) {
        groups.push_back(box);
        possibly_connected |= box.contains(from) && box.contains(to);
    }
    if (!possibly_connected) return false;
    Points candidates;
    for (const ExPolygon &part:centers) {
        auto add=[&](const Polygon &boundary) {
            for (const Point &p:boundary.points)
                if (p.x()>=std::min(from.x(),to.x())-reach && p.x()<=std::max(from.x(),to.x())+reach &&
                    p.y()>=std::min(from.y(),to.y())-reach && p.y()<=std::max(from.y(),to.y())+reach)
                    candidates.push_back(p);
        };
        add(part.contour);
        for (const auto &hole:part.holes) add(hole);
    }
    std::sort(candidates.begin(),candidates.end(),[&](const Point &a,const Point &b) {
        const double x=(a-from).cast<double>().norm()+(a-to).cast<double>().norm();
        const double y=(b-from).cast<double>().norm()+(b-to).cast<double>().norm();
        return std::make_tuple(x,a.x(),a.y())<std::make_tuple(y,b.x(),b.y());
    });
    if (candidates.size()>256) candidates.resize(256);
    Points nodes{from,to};
    std::set<Point> unique{from,to};
    for (const Point &p:candidates)
        if (unique.insert(p).second) nodes.push_back(p);
    if (nodes.size() == 2) return false;
    try {
        Clipper2Lib::Paths64 clip_paths;
        for (const Points &boundary : ClipperUtils::ExPolygonsProvider(visible)) {
            auto &path = clip_paths.emplace_back();
            path.reserve(boundary.size());
            for (const Point &point : boundary) path.emplace_back(point.x(), point.y());
        }
        prepared = std::make_unique<Clipper2Lib::ReuseableDataContainer64>();
        prepared->AddPaths(clip_paths, Clipper2Lib::PathType::Clip, false);
    } catch (const std::bad_alloc &) {
        prepared.reset();
    }
    std::vector<uint64_t> memberships(nodes.size(), 0);
    if (groups.size() <= 64)
        for (size_t i = 0; i < nodes.size(); ++i)
            for (size_t group = 0; group < groups.size(); ++group)
                if (groups[group].contains(nodes[i])) memberships[i] |= uint64_t(1) << group;
    std::vector<double> remaining(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) remaining[i] = (nodes[i] - to).cast<double>().norm();
    std::vector<double> distance(nodes.size(),std::numeric_limits<double>::max());
    std::vector<size_t> previous(nodes.size(),nodes.size());
    std::vector<bool> done(nodes.size(),false);
    size_t clip_vertices = 0;
    for (const ExPolygon &part : visible) {
        clip_vertices += part.contour.size();
        for (const Polygon &hole : part.holes) clip_vertices += hole.size();
    }
    const size_t requested_workers = nodes.size() < 32 || clip_vertices < 32768 / nodes.size() ? 1 :
        std::min({size_t(8), size_t(std::max(1u, std::thread::hardware_concurrency())),
            tbb::global_control::active_value(tbb::global_control::max_allowed_parallelism)});
    const size_t workspace = std::max(size_t(4) << 20, clip_vertices * size_t(512));
    SupportBatchBudget route_budget(requested_workers > 1 ? requested_workers * workspace : 0,
        requested_workers > 1 ? support_memory_snapshot() : SupportMemorySnapshot{});
    const size_t workers = std::max<size_t>(1, std::min(requested_workers, route_budget.bytes() / workspace));
    std::optional<tbb::task_arena> arena;
    if (workers > 1) arena.emplace(int(workers));
    distance[0]=0.;
    for (size_t step=0;step<nodes.size();++step) {
        size_t current=nodes.size();
        for (size_t i=0;i<nodes.size();++i)
            if (!done[i] && (current==nodes.size() || distance[i]<distance[current])) current=i;
        if (current==nodes.size() || distance[current]==std::numeric_limits<double>::max()) break;
        if (current==1) {
            Points route;
            for (size_t i=1;i!=0;i=previous[i]) route.push_back(nodes[i]);
            std::reverse(route.begin(),route.end());
            append(output,std::move(route));
            return true;
        }
        done[current]=true;
        // Keep the original Dijkstra settlement and tie order. A Euclidean
        // lower bound only removes edges that cannot improve an existing goal,
        // with a guard much larger than floating-point rounding at this scale.
        if (distance[current] + remaining[current] > distance[1] + 4 * SCALED_EPSILON) continue;
        std::vector<unsigned char> feasible;
        if (arena) {
            Points::size_type count = nodes.size();
            std::vector<size_t> queries;
            feasible.resize(count, 0);
            for (size_t next = 0; next < count; ++next) {
                if (done[next] || (groups.size() <= 64 && (memberships[current] & memberships[next]) == 0)) continue;
                const double cost = distance[current] + (nodes[next] - nodes[current]).cast<double>().norm();
                if (cost < distance[next] && cost + remaining[next] <= distance[1] + 4 * SCALED_EPSILON)
                    queries.push_back(next);
            }
            // Only independent visibility queries run concurrently. Relax the
            // resulting edges below in the unchanged original index order.
            arena->execute([&] {
                tbb::parallel_for(tbb::blocked_range<size_t>(0, queries.size(), 4), [&](const auto &range) {
                    for (size_t i = range.begin(); i < range.end(); ++i) {
                        const size_t next = queries[i];
                        feasible[next] = fits(nodes[current], nodes[next]);
                    }
                });
            });
        }
        for (size_t next=0;next<nodes.size();++next) {
            if (done[next]) continue;
            if (groups.size() <= 64 && (memberships[current] & memberships[next]) == 0) continue;
            const double cost=distance[current]+(nodes[next]-nodes[current]).cast<double>().norm();
            if (cost + remaining[next] > distance[1] + 4 * SCALED_EPSILON) continue;
            if (cost<distance[next] && (arena ? feasible[next] != 0 : fits(nodes[current],nodes[next]))) {
                distance[next]=cost;
                previous[next]=current;
            }
        }
    }
    return false;
}

size_t connect_support_interface_paths(ExtrusionEntityCollection &collection,
    const ExPolygons &allowed, float height, double max_link, const ExtrusionPaths &upper_paths,
    std::set<const ExtrusionEntity *> *changed)
{
    auto trace = [&](const char *event, size_t index, size_t other, double value) {
        if (const char *file = std::getenv("INLONG_INTERFACE_JOIN_TRACE")) {
            static std::mutex mutex;
            const std::lock_guard<std::mutex> lock(mutex);
            std::ofstream(file, std::ios::app) << "{\"event\":\"" << event << "\",\"index\":" << index
                << ",\"other\":" << other << ",\"height\":" << height << ",\"value\":" << value << "}\n";
        }
    };
    struct Chain {
        ExtrusionEntity **slot;
        ExtrusionPaths paths;
        bool reversible;
        size_t component;
        BoundingBox bounds;
        size_t vertices = 0;
        double total_length = 0.;
        bool closed = false;
    };
    // Bound offset complexity as well as graph size. Keeping the existing
    // roads is the safe fallback when dense geometry exceeds local planning.
    constexpr size_t max_road_vertices = 512, max_obstacle_vertices = 4096;
    auto vertex_count = [](const ExtrusionPaths &paths) {
        size_t count = 0;
        for (const ExtrusionPath &path : paths) count += path.polyline.size();
        return count;
    };
    std::vector<BoundingBox> component_bounds;
    for (const ExPolygon &part : allowed) component_bounds.push_back(get_extents(part));
    std::vector<Chain> chains;
    std::function<void(ExtrusionEntityCollection &)> gather = [&](ExtrusionEntityCollection &group) {
        for (ExtrusionEntity *&entity : group.entities) {
            if (auto *children = dynamic_cast<ExtrusionEntityCollection *>(entity)) {
                gather(*children);
                continue;
            }
            ExtrusionPaths paths;
            if (auto *path = dynamic_cast<ExtrusionPath *>(entity)) paths.push_back(*path);
            else if (auto *multi = dynamic_cast<ExtrusionMultiPath *>(entity)) paths = multi->paths;
            if (paths.empty()) continue;
            const ExtrusionPath &first = paths.front();
            bool valid = true;
            for (size_t i = 0; i < paths.size(); ++i) {
                const ExtrusionPath &path = paths[i];
                valid &= path.role() == erSupportMaterialInterface && !path.z_contoured &&
                    path.mm3_per_mm > 0. && path.height > 0.f && path.width >= path.height &&
                    std::abs(path.height - height) <= EPSILON &&
                    std::abs(path.width - first.width) <= EPSILON &&
                    std::abs(path.mm3_per_mm - first.mm3_per_mm) <= EPSILON &&
                    (i == 0 || paths[i - 1].last_point3() == path.first_point3());
            }
            if (!valid) continue;
            const Point point = first.first_point();
            size_t component = allowed.size();
            for (size_t i = 0; i < allowed.size(); ++i)
                if (component_bounds[i].contains(point) && allowed[i].contains(point)) { component = i; break; }
            if (component == allowed.size()) continue;
            BoundingBox bounds;
            for (const ExtrusionPath &path : paths)
                for (const Point3 &vertex : path.polyline.points) bounds.merge(vertex.to_point());
            chains.push_back({&entity, std::move(paths), entity->can_reverse(), component, bounds});
        }
    };
    gather(collection);
    trace("chains", chains.size(), allowed.size(), height);
    auto length = [](const ExtrusionPaths &paths) {
        double result = 0.;
        for (const ExtrusionPath &path : paths) result += path.length();
        return result;
    };
    auto reverse = [](ExtrusionPaths &paths) {
        std::reverse(paths.begin(), paths.end());
        for (ExtrusionPath &path : paths) path.reverse();
    };
    for (Chain &chain : chains) {
        chain.vertices = vertex_count(chain.paths);
        chain.total_length = length(chain.paths);
        chain.closed = chain.paths.front().first_point3() == chain.paths.back().last_point3();
    }
    // Candidate ordering remains the original chain index order. The grid only
    // removes chains whose expanded AABB cannot reach the short road; the exact
    // overlap and route tests below remain authoritative.
    constexpr int chain_grid_size = 64;
    BoundingBox chain_grid_bounds;
    for (const Chain &chain : chains)
        if (chain.bounds.defined)
            chain_grid_bounds.merge(chain.bounds);
    const bool chain_grid_enabled = chains.size() >= 64 && chain_grid_bounds.defined &&
        chain_grid_bounds.max.x() > chain_grid_bounds.min.x() &&
        chain_grid_bounds.max.y() > chain_grid_bounds.min.y();
    auto chain_grid_coordinate = [&](coord_t value, coord_t minimum, coord_t maximum) {
        const double normalized = double(value - minimum) / double(maximum - minimum);
        return std::clamp(int(normalized * chain_grid_size), 0, chain_grid_size - 1);
    };
    auto chain_grid_key = [](int x, int y) {
        return (uint64_t(uint32_t(x)) << 32) | uint32_t(y);
    };
    std::unordered_map<uint64_t, std::vector<size_t>> chain_grid;
    if (chain_grid_enabled) {
        for (size_t chain_index = 0; chain_index < chains.size(); ++chain_index) {
            const BoundingBox &bounds = chains[chain_index].bounds;
            if (!bounds.defined)
                continue;
            const int min_x = chain_grid_coordinate(bounds.min.x(), chain_grid_bounds.min.x(), chain_grid_bounds.max.x());
            const int max_x = chain_grid_coordinate(bounds.max.x(), chain_grid_bounds.min.x(), chain_grid_bounds.max.x());
            const int min_y = chain_grid_coordinate(bounds.min.y(), chain_grid_bounds.min.y(), chain_grid_bounds.max.y());
            const int max_y = chain_grid_coordinate(bounds.max.y(), chain_grid_bounds.min.y(), chain_grid_bounds.max.y());
            for (int x = min_x; x <= max_x; ++x)
                for (int y = min_y; y <= max_y; ++y)
                    chain_grid[chain_grid_key(x, y)].push_back(chain_index);
        }
    }
    auto slice = [](const ExtrusionPaths &paths, double begin, double end) {
        ExtrusionPaths result;
        double position = 0.;
        for (const ExtrusionPath &path : paths) {
            const double size = path.length();
            const double a = std::max(0., begin - position), b = std::min(size, end - position);
            if (b > a + SCALED_EPSILON) {
                ExtrusionPath part(path);
                part.polyline.clip_end(size - b);
                part.reverse(); part.polyline.clip_end(a); part.reverse();
                result.push_back(std::move(part));
            }
            position += size;
        }
        return result;
    };
    struct Projection { double position; double distance; };
    auto project = [](const Point3 &point, const ExtrusionPaths &paths) {
        Projection result{0., std::numeric_limits<double>::max()};
        double position = 0.;
        for (const ExtrusionPath &path : paths)
            for (size_t i = 1; i < path.polyline.points.size(); ++i) {
                const Vec3d a = path.polyline.points[i - 1].cast<double>();
                const Vec3d delta = (path.polyline.points[i] - path.polyline.points[i - 1]).cast<double>();
                const double size = delta.norm();
                if (size <= SCALED_EPSILON) continue;
                const double t = std::clamp((point.cast<double>() - a).dot(delta) / delta.squaredNorm(), 0., 1.);
                const double distance = (point.cast<double>() - a - t * delta).norm();
                if (distance < result.distance) result = {position + t * size, distance};
                position += size;
            }
        return result;
    };
    auto footprint = [](const ExtrusionPaths &paths) {
        Polygons result;
        for (const ExtrusionPath &path : paths) path.polygons_covered_by_width(result, 1.f);
        return union_(result);
    };
    size_t joined = 0;
    for (size_t small = 0; small < chains.size(); ++small) {
        if (chains[small].paths.empty()) continue;
        if (chains[small].vertices > max_road_vertices) continue;
        const ExtrusionPath flow = chains[small].paths.front();
        const double small_length = chains[small].total_length;
        if (small_length > 8. * scale_(flow.width)) continue;
        const ExtrusionPaths source = chains[small].paths;
        const bool source_closed = chains[small].closed;
        if (source_closed) continue;
        struct Candidate { double cost; size_t index; bool reversed; bool append; double a; double b; bool receiver_reversed = false; };
        std::vector<Candidate> candidates;
        std::vector<size_t> candidate_indices;
        if (chain_grid_enabled) {
            BoundingBox query = chains[small].bounds;
            query.offset(coord_t(std::ceil(max_link)) + 4 * SCALED_EPSILON);
            if (query.defined) {
                const int min_x = chain_grid_coordinate(query.min.x(), chain_grid_bounds.min.x(), chain_grid_bounds.max.x());
                const int max_x = chain_grid_coordinate(query.max.x(), chain_grid_bounds.min.x(), chain_grid_bounds.max.x());
                const int min_y = chain_grid_coordinate(query.min.y(), chain_grid_bounds.min.y(), chain_grid_bounds.max.y());
                const int max_y = chain_grid_coordinate(query.max.y(), chain_grid_bounds.min.y(), chain_grid_bounds.max.y());
                for (int x = min_x; x <= max_x; ++x)
                    for (int y = min_y; y <= max_y; ++y) {
                        const auto found = chain_grid.find(chain_grid_key(x, y));
                        if (found != chain_grid.end())
                            candidate_indices.insert(candidate_indices.end(), found->second.begin(), found->second.end());
                    }
                std::sort(candidate_indices.begin(), candidate_indices.end());
                candidate_indices.erase(std::unique(candidate_indices.begin(), candidate_indices.end()), candidate_indices.end());
            }
        }
        if (candidate_indices.empty()) {
            candidate_indices.resize(chains.size());
            std::iota(candidate_indices.begin(), candidate_indices.end(), size_t(0));
        }
        for (const size_t other : candidate_indices) {
            if (other == small || chains[other].paths.empty() ||
                chains[other].component != chains[small].component) continue;
            BoundingBox local = chains[small].bounds;
            local.offset(coord_t(std::ceil(max_link)));
            if (!local.overlap(chains[other].bounds)) continue;
            const ExtrusionPaths &receiver = chains[other].paths;
            if (chains[other].vertices > max_road_vertices) continue;
            const ExtrusionPath &other_flow = receiver.front();
            if (std::abs(flow.width - other_flow.width) > EPSILON ||
                std::abs(flow.mm3_per_mm - other_flow.mm3_per_mm) > EPSILON) continue;
            const bool closed = chains[other].closed;
            for (bool reversed : {false, true}) {
                if (reversed && !chains[small].reversible) continue;
                const Point3 a = reversed ? source.back().last_point3() : source.front().first_point3();
                const Point3 b = reversed ? source.front().first_point3() : source.back().last_point3();
                if (!closed)
                    for (bool receiver_reversed : {false, true}) {
                        if (receiver_reversed && !chains[other].reversible) continue;
                        const Point3 end = receiver_reversed ? receiver.front().first_point3() : receiver.back().last_point3();
                        const double gap = (end - a).cast<double>().norm();
                        if (gap <= max_link)
                            candidates.push_back({gap, other, reversed, true, 0., 0., receiver_reversed});
                    }
                if (chains[other].total_length <= small_length + 2. * scale_(flow.width)) continue;
                const Projection pa = project(a, receiver), pb = project(b, receiver);
                const double interval = pb.position - pa.position;
                if (std::max(pa.distance, pb.distance) <= max_link &&
                    (interval > SCALED_EPSILON || closed)) {
                    const double replaced = interval > 0. ? interval : chains[other].total_length + interval;
                    if (replaced <= small_length + 2. * max_link)
                        candidates.push_back({pa.distance + pb.distance, other, reversed, false, pa.position, pb.position});
                }
            }
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
            return std::tie(a.cost, a.index, a.reversed, a.append, a.a, a.b) <
                std::tie(b.cost, b.index, b.reversed, b.append, b.a, b.b);
        });
        // Bound geometric routing work on dense support layers. Retaining a
        // fragment is safer than searching unrelated parts of a large model.
        if (candidates.size() > 8) candidates.resize(8);
        trace("candidates", small, candidates.size(), unscale<double>(small_length));
        for (const Candidate &candidate : candidates) {
            ExtrusionPaths receiver = chains[candidate.index].paths;
            if (candidate.receiver_reversed) reverse(receiver);
            ExtrusionPaths finish(source);
            if (candidate.reversed) reverse(finish);
            // Only roads close to the changed interval can affect a connector
            // or mask a lost upper bearing. Clip their centerlines before
            // offsetting: a distant dense road can otherwise dominate slicing.
            BoundingBox window = chains[small].bounds;
            auto include = [&](const ExtrusionPaths &paths) {
                for (const ExtrusionPath &path : paths)
                    for (const Point3 &point : path.polyline.points) window.merge(point.to_point());
            };
            if (candidate.append)
                window.merge(receiver.back().last_point());
            else if (candidate.a < candidate.b)
                include(slice(receiver, candidate.a, candidate.b));
            else {
                include(slice(receiver, candidate.a, chains[candidate.index].total_length));
                include(slice(receiver, 0., candidate.b));
            }
            double bearing_width = flow.width;
            for (const ExtrusionPath &upper : upper_paths) bearing_width = std::max(bearing_width, double(upper.width));
            window.offset(coord_t(std::ceil(max_link + scale_(4. * bearing_width))) + 4 * SCALED_EPSILON);
            Polygons occupied;
            std::vector<std::pair<Polyline, float>> obstacles;
            size_t obstacle_vertices = 0;
            bool too_complex = false;
            std::function<void(const ExtrusionEntityCollection &)> add_others = [&](const ExtrusionEntityCollection &group) {
                for (const ExtrusionEntity *entity : group.entities) {
                    if (too_complex) break;
                    if (entity == *chains[small].slot || entity == *chains[candidate.index].slot || entity == nullptr) continue;
                    if (const auto *children = dynamic_cast<const ExtrusionEntityCollection *>(entity)) add_others(*children);
                    else for_each_extrusion_path(*entity, [&](const ExtrusionPath &path) {
                        if (too_complex) return;
                        const Polyline line = path.polyline.to_polyline();
                        if (line.size() < 2) return;
                        const BoundingBox bounds = get_extents(line);
                        BoundingBox guard = window;
                        // Keep artificial clipping caps well outside the
                        // queried region, including native square/miter joins.
                        guard.offset(coord_t(std::ceil(scale_(4. * std::abs(path.width)))) + 4 * SCALED_EPSILON);
                        if (!guard.overlap(bounds)) return;
                        const Polylines parts = guard.contains(bounds.min) && guard.contains(bounds.max) ?
                            Polylines{line} : intersection_pl(Polylines{line}, Polygons{guard.polygon()});
                        for (const Polyline &part : parts) {
                            obstacle_vertices += part.size();
                            if (part.size() > max_road_vertices || obstacle_vertices > max_obstacle_vertices) {
                                too_complex = true;
                                break;
                            }
                            obstacles.emplace_back(part, path.width);
                        }
                    });
                }
            };
            add_others(collection);
            if (too_complex) {
                trace("complex_obstacles", small, candidate.index, double(obstacle_vertices));
                continue;
            }
            for (const auto &[line, width] : obstacles)
                append(occupied, offset(line, float(scale_(width * 0.5)) + 1.f));
            occupied = intersection(occupied, Polygons{window.polygon()});
            Polygons original = footprint(receiver);
            polygons_append(original, footprint(finish));
            const ExPolygons material = diff_ex(union_ex(allowed, original), union_(occupied));
            // Inset into the material, rather than letting visibility epsilon
            // put a full-width bend outside a collision boundary. Existing
            // source centerlines remain valid ways out of a tightly fitted cap.
            ExPolygons centers = offset_ex(material, -0.5f * float(scale_(flow.width)) - 2.f * SCALED_EPSILON);
            for (const ExtrusionPaths *paths : {&receiver, &finish})
                for (const ExtrusionPath &path : *paths)
                    expolygons_append(centers, union_ex(offset(path.polyline.to_polyline(),
                        3.f * float(SCALED_EPSILON), jtRound, 1., etOpenRound)));
            centers = union_ex(centers);
            const ExPolygons visible = offset_ex(centers, float(SCALED_EPSILON));
            auto connector = [&](const Point3 &a, const Point3 &b, ExtrusionPaths &output) {
                if (a == b) return true;
                if ((a - b).cast<double>().norm() > max_link) return false;
                Points points{a.to_point()};
                // Every candidate node lies in this convex window. Clipping
                // only the visibility mask preserves the graph and its order,
                // while avoiding repeated booleans against an entire platform.
                BoundingBox window;
                window.merge(a.to_point()); window.merge(b.to_point());
                window.offset(coord_t(std::ceil(max_link)) + SCALED_EPSILON);
                const Polygon clip{Points{window.min, Point(window.max.x(), window.min.y()),
                    window.max, Point(window.min.x(), window.max.y())}};
                const ExPolygons local_visible = intersection_ex(visible, Polygons{clip});
                if (!route_support_path(a.to_point(), b.to_point(), centers, local_visible, max_link, points)) {
                    trace("no_route", small, candidate.index, unscale<double>((a - b).cast<double>().norm()));
                    return false;
                }
                Polyline line(std::move(points));
                if (line.length() > 1.5 * max_link) {
                    trace("route_length", small, candidate.index, unscale<double>(line.length()));
                    return false;
                }
                ExtrusionPath path(flow);
                path.polyline = Polyline3(std::move(line));
                const ExPolygons fit_material = union_ex(material, offset(original, 4.f * float(SCALED_EPSILON)));
                if (area(diff_ex(path.polygons_covered_by_width(), fit_material)) > sqr(double(scale_(0.002)))) {
                    trace("bead_outside", small, candidate.index, 0.);
                    return false;
                }
                double retraced = 0.;
                for (const Polyline &overlap : intersection_pl(Polylines{path.polyline.to_polyline()},
                        offset(original, -0.25f * float(scale_(flow.width))))) retraced += overlap.length();
                if (retraced > 1.5 * scale_(flow.width)) {
                    trace("retrace", small, candidate.index, unscale<double>(retraced));
                    return false;
                }
                output.push_back(std::move(path));
                return true;
            };
            ExtrusionPaths proposed;
            if (candidate.append) {
                proposed = receiver;
                if (!connector(proposed.back().last_point3(), finish.front().first_point3(), proposed)) continue;
                proposed.insert(proposed.end(), finish.begin(), finish.end());
            } else {
                const double size = chains[candidate.index].total_length;
                const bool closed = chains[candidate.index].closed;
                if (closed) {
                    proposed = slice(receiver, candidate.b, size);
                    auto remainder = slice(receiver, 0., candidate.a);
                    if (candidate.a > candidate.b) proposed = slice(receiver, candidate.b, candidate.a);
                    else proposed.insert(proposed.end(), remainder.begin(), remainder.end());
                    if (proposed.empty()) continue;
                    const Point3 end = proposed.front().first_point3();
                    if (!connector(proposed.back().last_point3(), finish.front().first_point3(), proposed)) continue;
                    proposed.insert(proposed.end(), finish.begin(), finish.end());
                    if (!connector(proposed.back().last_point3(), end, proposed)) continue;
                } else {
                    proposed = slice(receiver, 0., candidate.a);
                    auto tail = slice(receiver, candidate.b, size);
                    if (proposed.empty() || tail.empty()) continue;
                    if (!connector(proposed.back().last_point3(), finish.front().first_point3(), proposed)) continue;
                    proposed.insert(proposed.end(), finish.begin(), finish.end());
                    if (!connector(proposed.back().last_point3(), tail.front().first_point3(), proposed)) continue;
                    proposed.insert(proposed.end(), tail.begin(), tail.end());
                }
                const Polygons after = footprint(proposed);
                bool loses_bearing = false;
                if (upper_paths.empty()) {
                    loses_bearing = area(opening_ex(diff_ex(original, after), 0.5f * float(scale_(flow.height)))) >
                        sqr(double(SCALED_EPSILON));
                } else {
                    // The contact contour can take a local detour. Protect
                    // what its successor actually uses, rather than keeping
                    // every redundant inside corner of the old lower bead.
                    const Polygons before_all = union_(original, occupied);
                    const Polygons after_all = union_(after, occupied);
                    float expanded_width = -1.f;
                    Polygons before_expanded, after_expanded;
                    BoundingBox before_bounds;
                    for (const ExtrusionPath &upper : upper_paths) {
                        // Every upper road at the same width uses identical
                        // masks. Preserve their exact offset and input order,
                        // rather than rebuilding both masks for each road.
                        if (upper.width != expanded_width) {
                            before_expanded = offset(before_all, float(scale_(upper.width * 0.5) - SCALED_EPSILON));
                            after_expanded = offset(after_all, float(scale_(upper.width * 0.5)));
                            before_bounds = get_extents(before_expanded);
                            expanded_width = upper.width;
                        }
                        const Polyline upper_line = upper.polyline.to_polyline();
                        if (!before_bounds.defined || !before_bounds.overlap(get_extents(upper_line))) continue;
                        const Polylines was_supported = intersection_pl(Polylines{upper_line},
                            before_expanded);
                        for (const Polyline &lost : diff_pl(was_supported, after_expanded))
                            loses_bearing |= lost.length() > scale_(0.002);
                        if (loses_bearing) break;
                    }
                }
                if (loses_bearing) {
                    trace("coverage", small, candidate.index, 0.);
                    continue;
                }
            }
            auto *merged = new ExtrusionMultiPath;
            merged->paths = proposed;
            if (!chains[small].reversible || !chains[candidate.index].reversible) merged->set_reverse();
            if (changed != nullptr) {
                changed->erase(*chains[small].slot);
                changed->erase(*chains[candidate.index].slot);
                changed->insert(merged);
            }
            delete *chains[candidate.index].slot;
            *chains[candidate.index].slot = merged;
            delete *chains[small].slot;
            *chains[small].slot = nullptr;
            chains[candidate.index].paths = std::move(proposed);
            chains[candidate.index].bounds.merge(chains[small].bounds);
            chains[candidate.index].vertices = vertex_count(chains[candidate.index].paths);
            chains[candidate.index].total_length = length(chains[candidate.index].paths);
            chains[candidate.index].closed = chains[candidate.index].paths.front().first_point3() ==
                chains[candidate.index].paths.back().last_point3();
            chains[small].paths.clear();
            chains[small].vertices = 0;
            ++joined;
            trace("joined", small, candidate.index, 0.);
            break;
        }
    }
    std::function<void(ExtrusionEntityCollection &)> clean = [&](ExtrusionEntityCollection &group) {
        for (ExtrusionEntity *&entity : group.entities)
            if (auto *children = dynamic_cast<ExtrusionEntityCollection *>(entity)) {
                clean(*children);
                if (children->empty()) { delete children; entity = nullptr; }
            }
        group.entities.erase(std::remove(group.entities.begin(), group.entities.end(), nullptr), group.entities.end());
    };
    clean(collection);
    return joined;
}

size_t modulate_joined_support_paths(SupportLayerPtrs &layers, float minimum_height,
    const std::set<const ExtrusionEntity *> &changed)
{
    if (changed.empty()) return 0;
    const auto profile_started = std::chrono::steady_clock::now();
    double max_height = 0.;
    for (const SupportLayer *layer : layers)
        for_each_extrusion_path(layer->support_fills, [&](const ExtrusionPath &path) {
            max_height = std::max(max_height, double(path.height));
        });
    struct PrintedLayer {
        SupportGeneratorLayer source;
        const SupportLayer *printed = nullptr;
        BoundingBox printed_bounds;
        bool initialized = false;
    };
    std::deque<PrintedLayer> recent;
    size_t adjusted = 0;
    for (SupportLayer *layer : layers) {
        while (!recent.empty() && recent.front().source.print_z < layer->print_z - max_height - EPSILON)
            recent.pop_front();
        std::function<void(ExtrusionEntityCollection &)> modulate = [&](ExtrusionEntityCollection &group) {
            for (ExtrusionEntity *&entity : group.entities) {
                if (auto *children = dynamic_cast<ExtrusionEntityCollection *>(entity)) {
                    modulate(*children);
                    continue;
                }
                if (changed.count(entity) == 0) continue;
                ExtrusionPaths result;
                bool altered = false;
                for_each_extrusion_path(*entity, [&](const ExtrusionPath &path) {
                    SupportGeneratorLayersPtr overlaps;
                    // Most support layers already meet exactly at their Z
                    // boundaries. They need no geometric height modulation.
                    // Keep earlier material lazy until an actual vertical
                    // overlap query needs it; completed lower layers are stable.
                    std::vector<PrintedLayer *> candidates;
                    const BoundingBox path_bounds = get_extents(path.polyline.to_polyline()).inflated(
                        0.5 * scale_(std::abs(path.width)) + SCALED_EPSILON);
                    for (PrintedLayer &below : recent) {
                        if (below.source.print_z <= layer->print_z - path.height + EPSILON ||
                            layer->print_z - below.source.print_z < minimum_height - EPSILON)
                            continue;
                        // support_islands is already assembled for every
                        // printed layer. Reject clearly disjoint layers
                        // before constructing bead polygons and running a
                        // Clipper intersection. Empty islands retain the
                        // exact historical path because there is no safe
                        // rejection envelope.
                        if (below.printed_bounds.defined && path_bounds.defined &&
                            !below.printed_bounds.overlap(path_bounds))
                            continue;
                        candidates.push_back(&below);
                    }
                    if (candidates.empty()) { result.push_back(path); return; }
                    const Polygons bead = path.polygons_covered_by_width();
                    for (PrintedLayer *below : candidates) {
                        if (!below->initialized) {
                            for_each_extrusion_path(below->printed->support_fills, [&](const ExtrusionPath &printed) {
                                if (!printed.is_force_no_extrusion() && printed.mm3_per_mm > 0.)
                                    printed.polygons_covered_by_width(below->source.polygons, float(SCALED_EPSILON));
                            });
                            below->source.polygons = union_(below->source.polygons);
                            below->initialized = true;
                        }
                        if (area(intersection(bead, below->source.polygons)) > sqr(double(scale_(0.001))))
                            overlaps.push_back(&below->source);
                    }
                    if (overlaps.empty()) { result.push_back(path); return; }
                    SupportGeneratorLayer source;
                    source.print_z = layer->print_z;
                    source.bottom_z = layer->print_z - path.height;
                    source.height = path.height;
                    ExtrusionEntitiesPtr fragments{new ExtrusionPath(path)};
                    modulate_extrusion_by_overlapping_layers(fragments, source, overlaps);
                    for (ExtrusionEntity *fragment : fragments) {
                        for_each_extrusion_path(*fragment, [&](const ExtrusionPath &part) { result.push_back(part); });
                        delete fragment;
                    }
                    altered = true;
                });
                if (altered) {
                    auto *replacement = new ExtrusionMultiPath;
                    replacement->paths = std::move(result);
                    if (!entity->can_reverse()) replacement->set_reverse();
                    delete entity;
                    entity = replacement;
                    ++adjusted;
                }
            }
        };
        modulate(layer->support_fills);
        recent.emplace_back();
        recent.back().source.print_z = layer->print_z;
        recent.back().printed = layer;
        if (!layer->support_islands.empty())
            recent.back().printed_bounds = get_extents(layer->support_islands);
    }
    BOOST_LOG_TRIVIAL(info) << "Organic physical-height modulation: " <<
        std::chrono::duration<double>(std::chrono::steady_clock::now() - profile_started).count() <<
        " seconds, adjusted=" << adjusted;
    return adjusted;
}

size_t generate_support_toolpaths(
    SupportLayerPtrs                    &support_layers,
    const PrintObjectConfig             &config,
    const SupportParameters             &support_params,
    const SlicingParameters             &slicing_params,
    const SupportGeneratorLayersPtr     &raft_layers,
    const SupportGeneratorLayersPtr     &bottom_contacts,
    const SupportGeneratorLayersPtr     &top_contacts,
    const SupportGeneratorLayersPtr     &intermediate_layers,
    const SupportGeneratorLayersPtr     &interface_layers,
    const SupportGeneratorLayersPtr     &base_interface_layers)
{
    size_t physical_height_adjustments = 0;
    // loop_interface_processor with a given circle radius.
    LoopInterfaceProcessor loop_interface_processor(1.5 * support_params.support_material_interface_flow.scaled_width());
    loop_interface_processor.n_contact_loops = config.support_interface_loop_pattern.value ? 1 : 0;

    std::vector<float>      angles { support_params.base_angle };
    if (config.support_base_pattern == smpRectilinearGrid)
        angles.push_back(support_params.interface_angle);
    std::vector<float>      raft_angles { support_params.raft_angle_base };
    if (config.raft_base_pattern == smpRectilinearGrid)
        raft_angles.push_back(support_params.raft_angle_interface);

    BoundingBox bbox_object(Point(-scale_(1.), -scale_(1.0)), Point(scale_(1.), scale_(1.)));

//    const coordf_t link_max_length_factor = 3.;
    const coordf_t link_max_length_factor = 0.;

    // Insert the raft base layers.
    auto n_raft_layers = std::min<size_t>(support_layers.size(), std::max(0, int(slicing_params.raft_layers()) - 1));

    tbb::parallel_for(tbb::blocked_range<size_t>(0, n_raft_layers),
        [&support_layers, &raft_layers, &intermediate_layers, &config, &support_params, &slicing_params,
            &bbox_object, &raft_angles, link_max_length_factor]
            (const tbb::blocked_range<size_t>& range) {
        for (size_t support_layer_id = range.begin(); support_layer_id < range.end(); ++ support_layer_id)
        {
            assert(support_layer_id < raft_layers.size());
            SupportLayer               &support_layer = *support_layers[support_layer_id];
            assert(support_layer.support_fills.entities.empty());
            SupportGeneratorLayer      &raft_layer    = *raft_layers[support_layer_id];

            std::unique_ptr<Fill> filler_interface = std::unique_ptr<Fill>(Fill::new_from_type(support_params.raft_interface_fill_pattern));
            std::unique_ptr<Fill> filler_support   = std::unique_ptr<Fill>(Fill::new_from_type(support_params.raft_base_fill_pattern));
            filler_interface->set_bounding_box(bbox_object);
            filler_support->set_bounding_box(bbox_object);

            Polygons raft_support_regions;
            Polygons raft_interface_regions;
            auto store_raft_regions = [&]() {
                support_layer.raft_support_islands = union_ex(raft_support_regions);
                support_layer.raft_interface_islands = union_ex(raft_interface_regions);
                ExPolygons raft_islands = support_layer.raft_support_islands;
                expolygons_append(raft_islands, support_layer.raft_interface_islands);
                support_layer.support_islands = union_ex(raft_islands);
            };

            // Print the tree supports cutting through the raft with the exception of the 1st layer, where a full support layer will be printed below
            // both the raft and the trees.
            // Trim the raft layers with the tree polygons.
            const Polygons &tree_polygons =
                support_layer_id > 0 && support_layer_id < intermediate_layers.size() && is_approx(intermediate_layers[support_layer_id]->print_z, support_layer.print_z) ?
                intermediate_layers[support_layer_id]->polygons : Polygons();

            // Print the support base below the support columns, or the support base for the support columns plus the contacts.
            if (support_layer_id > 0) {
                const Polygons &to_infill_polygons = (support_layer_id < slicing_params.base_raft_layers) ?
                    raft_layer.polygons :
                    //FIXME misusing contact_polygons for support columns.
                    ((raft_layer.contact_polygons == nullptr) ? Polygons() : *raft_layer.contact_polygons);
                // Trees may cut through the raft layers down to a print bed.
                Flow flow(float(support_params.support_material_flow.width()), float(raft_layer.height), support_params.support_material_flow.nozzle_diameter());
                assert(!raft_layer.bridging);
                if (! to_infill_polygons.empty()) {
                    Polygons support_region = tree_polygons.empty() ?
                        to_infill_polygons : diff(to_infill_polygons, tree_polygons);
                    Fill *filler = filler_support.get();
                    filler->angle = raft_angles[support_layer_id % raft_angles.size()];
                    filler->spacing = support_params.support_material_flow.spacing();
                    filler->link_max_length = coord_t(scale_(filler->spacing * link_max_length_factor / support_params.raft_base_density));
                    polygons_append(raft_support_regions, support_region);
                    fill_expolygons_with_sheath_generate_paths(
                        // Destination
                        support_layer.support_fills.entities,
                        // Regions to fill
                        support_region,
                        // Filler and its parameters
                        filler, float(support_params.raft_base_density),
                        // Extrusion parameters
                        ExtrusionRole::erSupportMaterial, flow,
                        support_params, support_params.with_sheath, false,
                        support_params.raft_base_fill_pattern == ipConcentric);
                }
                if (! tree_polygons.empty()) {
                    polygons_append(raft_support_regions, tree_polygons);
                    tree_supports_generate_paths(support_layer.support_fills.entities, tree_polygons, flow, support_params);
                }
            }

            Fill *filler = filler_interface.get();
            Flow  flow = support_params.first_layer_flow;
            float density = 0.f;
            if (support_layer_id == 0) {
                // Base flange.
                filler->angle = support_params.raft_angle_1st_layer;
                filler->spacing = support_params.first_layer_flow.spacing();
                density       = float(config.raft_first_layer_density.value * 0.01);
            } else if (support_layer_id >= slicing_params.base_raft_layers) {
                filler->angle = support_params.raft_interface_angle(support_layer.interface_id());
                // We don't use $base_flow->spacing because we need a constant spacing
                // value that guarantees that all layers are correctly aligned.
                filler->spacing = support_params.support_material_flow.spacing();
                assert(! raft_layer.bridging);
                flow          = Flow(float(support_params.raft_interface_flow.width()), float(raft_layer.height), support_params.raft_interface_flow.nozzle_diameter());
                density       = float(support_params.raft_interface_density);
            } else {
                store_raft_regions();
                continue;
            }
            filler->link_max_length = coord_t(scale_(filler->spacing * link_max_length_factor / density));
            Polygons raft_region = tree_polygons.empty() ?
                raft_layer.polygons : diff(raft_layer.polygons, tree_polygons);
            if (support_layer_id < slicing_params.base_raft_layers)
                polygons_append(raft_support_regions, raft_region);
            else
                polygons_append(raft_interface_regions, raft_region);
            fill_expolygons_with_sheath_generate_paths(
                // Destination
                support_layer.support_fills.entities,
                // Regions to fill
                raft_region,
                // Filler and its parameters
                filler, density,
                // Extrusion parameters
                (support_layer_id < slicing_params.base_raft_layers) ? ExtrusionRole::erSupportMaterial : ExtrusionRole::erSupportMaterialInterface, flow,
                // sheath at first layer
                support_params, support_layer_id == 0, support_layer_id == 0,
                support_params.raft_interface_fill_pattern == ipConcentric);

            store_raft_regions();
        }
    });

    struct LayerCacheItem {
        LayerCacheItem(SupportGeneratorLayerExtruded *layer_extruded = nullptr) : layer_extruded(layer_extruded) {}
        SupportGeneratorLayerExtruded         *layer_extruded;
        std::vector<SupportGeneratorLayer*>    overlapping;
    };
    struct LayerCache {
        SupportGeneratorLayerExtruded                                     bottom_contact_layer;
        SupportGeneratorLayerExtruded                                     top_contact_layer;
        SupportGeneratorLayerExtruded                                     base_layer;
        SupportGeneratorLayerExtruded                                     interface_layer;
        SupportGeneratorLayerExtruded                                     base_interface_layer;
        boost::container::static_vector<LayerCacheItem, 5>  nonempty;

        float    ironing_angle;
        Polygons polys_to_iron;

        void add_nonempty_and_sort() {
            for (SupportGeneratorLayerExtruded *item : { &bottom_contact_layer, &top_contact_layer, &interface_layer, &base_interface_layer, &base_layer })
                if (! item->empty())
                    this->nonempty.emplace_back(item);
            // Sort the layers with the same print_z coordinate by their heights, thickest first.
            std::stable_sort(this->nonempty.begin(), this->nonempty.end(), [](const LayerCacheItem &lc1, const LayerCacheItem &lc2) { return lc1.layer_extruded->layer->height > lc2.layer_extruded->layer->height; });
        }
    };
    std::vector<LayerCache>             layer_caches(support_layers.size());

    tbb::parallel_for(tbb::blocked_range<size_t>(n_raft_layers, support_layers.size()),
        [&config, &slicing_params, &support_params, &support_layers, &bottom_contacts, &top_contacts, &intermediate_layers, &interface_layers, &base_interface_layers, &layer_caches, &loop_interface_processor,
            &bbox_object, &angles, n_raft_layers, link_max_length_factor]
            (const tbb::blocked_range<size_t>& range) {
        // Indices of the 1st layer in their respective container at the support layer height.
        size_t idx_layer_bottom_contact   = size_t(-1);
        size_t idx_layer_top_contact      = size_t(-1);
        size_t idx_layer_intermediate     = size_t(-1);
        size_t idx_layer_interface        = size_t(-1);
        size_t idx_layer_base_interface   = size_t(-1);
        const auto fill_type_first_layer  = ipRectilinear;
        auto interface_pattern = [&](InfillPattern pattern) {
            // Sparse organic interfaces are platforms, not structural trunks.
            // The legacy density fallback chooses SupportBase, which adds
            // arches and return roads instead of the configured straight rows.
            return support_params.support_style == smsTreeOrganic && pattern == ipSupportBase ?
                ipRectilinear : pattern;
        };
        const InfillPattern top_contact_fill_pattern = interface_pattern(support_params.top_contact_fill_pattern);
        const InfillPattern bottom_contact_fill_pattern = interface_pattern(support_params.bottom_contact_fill_pattern);
        const InfillPattern interface_fill_pattern = interface_pattern(support_params.interface_fill_pattern);
        auto filler_top_contact     = std::unique_ptr<Fill>(Fill::new_from_type(top_contact_fill_pattern));
        auto filler_bottom_contact  = std::unique_ptr<Fill>(Fill::new_from_type(bottom_contact_fill_pattern));
        auto filler_interface       = std::unique_ptr<Fill>(Fill::new_from_type(interface_fill_pattern));
        // Filler for the 1st layer interface, if different from filler_interface.
        auto filler_first_layer_ptr = std::unique_ptr<Fill>(range.begin() == 0 && top_contact_fill_pattern != fill_type_first_layer ? Fill::new_from_type(fill_type_first_layer) : nullptr);
        // Pointer to the 1st layer interface filler.
        auto filler_first_layer     = filler_first_layer_ptr ? filler_first_layer_ptr.get() : filler_top_contact.get();
        // Filler for the 1st layer interface, if different from filler_interface.
        const bool top_interfaces_enabled    = support_params.num_top_interface_layers > 0;
        const bool bottom_interfaces_enabled = support_params.num_bottom_interface_layers > 0;
        const coordf_t base_interface_density = top_interfaces_enabled || !bottom_interfaces_enabled ?
            support_params.top_interface_density : support_params.bottom_interface_density;
        auto filler_raft_contact_ptr = std::unique_ptr<Fill>(range.begin() == n_raft_layers && !top_interfaces_enabled ?
            Fill::new_from_type(support_params.raft_interface_fill_pattern) : nullptr);
        // Pointer to the 1st layer interface filler.
        auto filler_raft_contact     = filler_raft_contact_ptr ? filler_raft_contact_ptr.get() : filler_interface.get();
        // Filler for the base interface (to be used for soluble interface / non soluble base, to produce non soluble interface layer below soluble interface layer).
        auto filler_base_interface  = std::unique_ptr<Fill>(base_interface_layers.empty() ? nullptr :
            Fill::new_from_type(base_interface_density > 0.95 || support_params.with_sheath ? ipRectilinear : ipSupportBase));
        auto filler_support         = std::unique_ptr<Fill>(Fill::new_from_type(support_params.base_fill_pattern));
        filler_top_contact->set_bounding_box(bbox_object);
        filler_bottom_contact->set_bounding_box(bbox_object);
        filler_interface->set_bounding_box(bbox_object);
        if (filler_first_layer_ptr)
            filler_first_layer_ptr->set_bounding_box(bbox_object);
        if (filler_raft_contact_ptr)
            filler_raft_contact_ptr->set_bounding_box(bbox_object);
        if (filler_base_interface)
            filler_base_interface->set_bounding_box(bbox_object);
        filler_support->set_bounding_box(bbox_object);
        for (size_t support_layer_id = range.begin(); support_layer_id < range.end(); ++ support_layer_id)
        {
            SupportLayer &support_layer = *support_layers[support_layer_id];
            LayerCache   &layer_cache   = layer_caches[support_layer_id];
            const float support_interface_angle = support_params.support_interface_angle(
                support_params.interface_pattern, support_layer.interface_id());
            const float support_top_contact_angle = support_params.support_interface_angle(
                support_params.top_contact_pattern, support_layer.interface_id());

            // Find polygons with the same print_z.
            SupportGeneratorLayerExtruded &bottom_contact_layer = layer_cache.bottom_contact_layer;
            SupportGeneratorLayerExtruded &top_contact_layer    = layer_cache.top_contact_layer;
            SupportGeneratorLayerExtruded &base_layer           = layer_cache.base_layer;
            SupportGeneratorLayerExtruded &interface_layer      = layer_cache.interface_layer;
            SupportGeneratorLayerExtruded &base_interface_layer = layer_cache.base_interface_layer;
            // Increment the layer indices to find a layer at support_layer.print_z.
            {
                auto fun = [&support_layer](const SupportGeneratorLayer *l){ return l->print_z >= support_layer.print_z - EPSILON; };
                idx_layer_bottom_contact  = idx_higher_or_equal(bottom_contacts,     idx_layer_bottom_contact,  fun);
                idx_layer_top_contact     = idx_higher_or_equal(top_contacts,        idx_layer_top_contact,     fun);
                idx_layer_intermediate    = idx_higher_or_equal(intermediate_layers, idx_layer_intermediate,    fun);
                idx_layer_interface       = idx_higher_or_equal(interface_layers,    idx_layer_interface,       fun);
                idx_layer_base_interface  = idx_higher_or_equal(base_interface_layers, idx_layer_base_interface,fun);
            }
            // Copy polygons from the layers.
            if (idx_layer_bottom_contact < bottom_contacts.size() && bottom_contacts[idx_layer_bottom_contact]->print_z < support_layer.print_z + EPSILON)
                bottom_contact_layer.layer = bottom_contacts[idx_layer_bottom_contact];
            if (idx_layer_top_contact < top_contacts.size() && top_contacts[idx_layer_top_contact]->print_z < support_layer.print_z + EPSILON)
                top_contact_layer.layer = top_contacts[idx_layer_top_contact];
            if (idx_layer_interface < interface_layers.size() && interface_layers[idx_layer_interface]->print_z < support_layer.print_z + EPSILON)
                interface_layer.layer = interface_layers[idx_layer_interface];
            if (idx_layer_base_interface < base_interface_layers.size() && base_interface_layers[idx_layer_base_interface]->print_z < support_layer.print_z + EPSILON)
                base_interface_layer.layer = base_interface_layers[idx_layer_base_interface];
            if (idx_layer_intermediate < intermediate_layers.size() && intermediate_layers[idx_layer_intermediate]->print_z < support_layer.print_z + EPSILON)
                base_layer.layer = intermediate_layers[idx_layer_intermediate];

            // This layer is a raft contact layer. Any contact polygons at this layer are raft contacts.
            bool raft_layer = slicing_params.interface_raft_layers && top_contact_layer.layer && is_approx(top_contact_layer.layer->print_z, slicing_params.raft_contact_top_z);
            // INLONG: Organic tree uses projected contacts to build the interface stack; avoid extra bottom-contact extrusion.
            const bool organic_tree = support_params.support_style == SupportMaterialStyle::smsTreeOrganic;
            const bool top_interfaces = support_params.num_top_interface_layers > 0;
            const bool bottom_interfaces = support_params.num_bottom_interface_layers > 0;
            const bool top_contact_matches_interface =
                top_contact_fill_pattern == interface_fill_pattern &&
                std::abs(support_params.top_contact_density -
                    (!interface_layer.empty() && interface_layer.layer->layer_type == SupporLayerType::BottomInterface ?
                     support_params.bottom_interface_density : support_params.top_interface_density)) < EPSILON &&
                support_params.top_contact_wall_count ==
                    (!interface_layer.empty() && interface_layer.layer->layer_type == SupporLayerType::BottomInterface ?
                     support_params.bottom_interface_wall_count : support_params.top_interface_wall_count);
            const bool bottom_contact_matches_interface =
                bottom_contact_fill_pattern == interface_fill_pattern &&
                std::abs(support_params.bottom_contact_density - support_params.bottom_interface_density) < EPSILON &&
                support_params.bottom_contact_wall_count == support_params.bottom_interface_wall_count;
            const bool top_bottom_contacts_match =
                top_contact_fill_pattern == bottom_contact_fill_pattern &&
                std::abs(support_params.top_contact_density - support_params.bottom_contact_density) < EPSILON &&
                support_params.top_contact_wall_count == support_params.bottom_contact_wall_count;
            if (!top_interfaces) {
                // If no top interface layers were requested, we treat the contact layer exactly as a generic base layer.
                // Don't merge the raft contact layer though.
                if (support_params.can_merge_support_regions && ! raft_layer) {
                    if (base_layer.could_merge(top_contact_layer))
                        base_layer.merge(std::move(top_contact_layer));
                    else if (base_layer.empty())
                        base_layer = std::move(top_contact_layer);
                }
            } else {
                if (support_params.ironing && !top_contact_layer.empty()) {
                    // Inlong: save the top surface to be ironed later
                    layer_cache.ironing_angle = support_top_contact_angle; // TODO: should we rotate 90 degrees?
                    layer_cache.polys_to_iron = top_contact_layer.polygons_to_extrude();
                }

                loop_interface_processor.generate(top_contact_layer, support_params.support_material_interface_flow);
                // If no loops are allowed, we treat the contact layer exactly as a generic interface layer.
                // Merge interface_layer into top_contact_layer, as the top_contact_layer is not synchronized and therefore it will be used
                // to trim other layers.
                if (top_contact_layer.could_merge(interface_layer) && ! raft_layer && top_contact_matches_interface)
                    top_contact_layer.merge(std::move(interface_layer));
            }
            // Bottom contacts are real support-contact surfaces. Keep the
            // normal merge/difference rules for organic trees as well; using
            // them only as projection seeds drops the entire bottom contact
            // toolpath before extrusion.
            {
                if (!bottom_interfaces && support_params.can_merge_support_regions) {
                    if (base_layer.could_merge(bottom_contact_layer))
                        base_layer.merge(std::move(bottom_contact_layer));
                    else if (base_layer.empty() && ! bottom_contact_layer.empty() && ! bottom_contact_layer.layer->bridging)
                        base_layer = std::move(bottom_contact_layer);
                } else if (bottom_contact_layer.could_merge(top_contact_layer) && ! raft_layer) {
                    if (top_interfaces && bottom_interfaces && top_bottom_contacts_match) {
                        top_contact_layer.merge(std::move(bottom_contact_layer));
                    } else if (bottom_interfaces) {
                        top_contact_layer.set_polygons_to_extrude(
                            diff(top_contact_layer.polygons_to_extrude(), bottom_contact_layer.polygons_to_extrude()));
                    } else {
                        bottom_contact_layer.set_polygons_to_extrude(
                            diff(bottom_contact_layer.polygons_to_extrude(), top_contact_layer.polygons_to_extrude()));
                    }
                } else if (bottom_contact_layer.could_merge(interface_layer)) {
                    const bool interface_layer_is_bottom = interface_layer.layer->layer_type == SupporLayerType::BottomInterface;
                    if (bottom_interfaces && interface_layer_is_bottom && bottom_contact_matches_interface) {
                        bottom_contact_layer.merge(std::move(interface_layer));
                    } else {
                        bottom_contact_layer.set_polygons_to_extrude(
                            diff(bottom_contact_layer.polygons_to_extrude(), interface_layer.polygons_to_extrude()));
                    }
                }
            }

            // Organic tree support-material regions are generated from expanded
            // wall polygons. Keep them separate from every same-Z interface
            // footprint so interface paths cannot be emitted through the trunk.
            if (organic_tree &&
                (!base_layer.empty() || !base_interface_layer.empty())) {
                Polygons interface_polygons;
                if (!top_contact_layer.empty())
                    polygons_append(interface_polygons, top_contact_layer.polygons_to_extrude());
                if (!bottom_contact_layer.empty()) {
                    polygons_append(interface_polygons, bottom_contact_layer.polygons_to_extrude());
                    if (support_params.bottom_contact_wall_count > 0 && bottom_contact_layer.layer->contact_polygons)
                        polygons_append(interface_polygons, *bottom_contact_layer.layer->contact_polygons);
                }
                if (!interface_layer.empty())
                    polygons_append(interface_polygons, interface_layer.polygons_to_extrude());
                if (!interface_polygons.empty()) {
                    const coord_t trim_margin = std::max(
                        support_params.support_material_flow.scaled_width(),
                        support_params.support_material_interface_flow.scaled_width()) / 2;
                    Polygons interface_keepout = offset(interface_polygons, trim_margin);
                    if (!base_layer.empty())
                        base_layer.set_polygons_to_extrude(diff(base_layer.polygons_to_extrude(), interface_keepout));
                    if (!base_interface_layer.empty())
                        base_interface_layer.set_polygons_to_extrude(diff(base_interface_layer.polygons_to_extrude(), interface_keepout));
                }
            }

#if 0
            if ( ! interface_layer.empty() && ! base_layer.empty()) {
                // turn base support into interface when it's contained in our holes
                // (this way we get wider interface anchoring)
                //FIXME The intention of the code below is unclear. One likely wanted to just merge small islands of base layers filling in the holes
                // inside interface layers, but the code below fills just too much, see GH #4570
                Polygons islands = top_level_islands(interface_layer.layer->polygons);
                polygons_append(interface_layer.layer->polygons, intersection(base_layer.layer->polygons, islands));
                base_layer.layer->polygons = diff(base_layer.layer->polygons, islands);
            }
#endif

            // Top and bottom contacts, interface layers.
            enum class InterfaceLayerType { TopContact, BottomContact, RaftContact, Interface, InterfaceAsBase };
            // A printed contact owns its whole effective pattern region,
            // including intentional line-spacing voids. Generic interfaces
            // must not refill those voids with isolated residual strokes.
            // Actual beads also protect perimeter portions outside the patches.
            Polygons contact_keepout;
            auto extrude_interface = [&](SupportGeneratorLayerExtruded &layer_ex, InterfaceLayerType interface_layer_type) {
                if (! layer_ex.empty() && ! layer_ex.polygons_to_extrude().empty()) {
                    bool interface_as_base = interface_layer_type == InterfaceLayerType::InterfaceAsBase;
                    bool raft_contact      = interface_layer_type == InterfaceLayerType::RaftContact;
                    // INLONG: detect bottom interface layers for density selection.
                    bool bottom_interface  = interface_layer_type == InterfaceLayerType::BottomContact ||
                        (interface_layer_type == InterfaceLayerType::Interface && layer_ex.layer->layer_type == SupporLayerType::BottomInterface);
                    //FIXME Bottom interfaces are extruded with the briding flow. Some bridging layers have its height slightly reduced, therefore
                    // the bridging flow does not quite apply. Reduce the flow to area of an ellipse? (A = pi * a * b)
                    auto *filler = raft_contact ? filler_raft_contact :
                        interface_layer_type == InterfaceLayerType::TopContact ? filler_top_contact.get() :
                        interface_layer_type == InterfaceLayerType::BottomContact ? filler_bottom_contact.get() :
                        filler_interface.get();
                    auto interface_flow = layer_ex.layer->bridging ?
                        Flow::bridging_flow(layer_ex.layer->height, support_params.support_material_bottom_interface_flow.nozzle_diameter()) :
                        (raft_contact ? &support_params.raft_interface_flow :
                         interface_as_base ? &support_params.support_material_flow : &support_params.support_material_interface_flow)
                            ->with_height(float(layer_ex.layer->height));
                    const SupportMaterialInterfacePattern pattern =
                        interface_layer_type == InterfaceLayerType::TopContact ? support_params.top_contact_pattern :
                        interface_layer_type == InterfaceLayerType::BottomContact ? support_params.bottom_contact_pattern :
                        support_params.interface_pattern;
                    const InfillPattern fill_pattern = raft_contact ? support_params.raft_interface_fill_pattern :
                        interface_layer_type == InterfaceLayerType::TopContact ? top_contact_fill_pattern :
                        interface_layer_type == InterfaceLayerType::BottomContact ? bottom_contact_fill_pattern :
                        interface_fill_pattern;
                    const InfillPattern legacy_fill_pattern = raft_contact ? support_params.raft_interface_fill_pattern :
                        interface_layer_type == InterfaceLayerType::TopContact ? support_params.top_contact_fill_pattern :
                        interface_layer_type == InterfaceLayerType::BottomContact ? support_params.bottom_contact_fill_pattern :
                        support_params.interface_fill_pattern;
                    filler->angle = interface_as_base ?
                            // If zero interface layers are configured, use the same angle as for the base layers.
                            angles[support_layer_id % angles.size()] :
                            // Use interface angle for the interface layers.
                            raft_contact ?
                                support_params.raft_interface_angle(support_layer.interface_id()) :
                                support_params.support_interface_angle(pattern, support_layer.interface_id());
                    // INLONG: pick density based on interface type.
                    double density = raft_contact ? support_params.raft_interface_density :
                        interface_as_base ? support_params.support_density :
                        interface_layer_type == InterfaceLayerType::TopContact ? support_params.top_contact_density :
                        interface_layer_type == InterfaceLayerType::BottomContact ? support_params.bottom_contact_density :
                        bottom_interface ? support_params.bottom_interface_density : support_params.top_interface_density;
                    filler->spacing = raft_contact ? support_params.raft_interface_flow.spacing() :
                        interface_as_base ? support_params.support_material_flow.spacing() : support_params.support_material_interface_flow.spacing();
                    // Organic contact/interface regions are often split into
                    // adjacent clipped polygons around a sharp overhang. A
                    // zero link length leaves every short fill segment
                    // isolated, even when the connector is fully inside the
                    // collision-safe interface polygon. Keep the existing
                    // base-support behavior, but allow the interface filler
                    // to stitch nearby segments into one printable platform.
                    const coordf_t interface_link_factor =
                        organic_tree && !interface_as_base ? 3. : link_max_length_factor;
                    filler->link_max_length = coord_t(scale_(filler->spacing * interface_link_factor / density));
                    // Concentric contact surfaces can collapse into a narrow
                    // center remainder at a sharp tip.  The center paths are
                    // safe because they are clipped to the already collision-
                    // checked contact polygon, and are required for top and
                    // bottom contacts as well as raft contacts.  Without
                    // this, only raft contact layers fill the remainder and
                    // the model-side contact platform has a longitudinal gap.
                    const bool fill_concentric_contact_gaps =
                        fill_pattern == ipConcentric;
                    Polygons interface_fill_polygons = layer_ex.polygons_to_extrude();
                    if (organic_tree && !interface_as_base && !interface_fill_polygons.empty()) {
                        // Tree contact regions can be split by sub-nozzle
                        // clipping at a sharp corner. Close only a half-line-
                        // width seam before filling, so neighboring pieces
                        // become one platform while the change remains below
                        // one bead on either side of the seam.
                        const coord_t closing_distance = std::max<coord_t>(
                            1, interface_flow.scaled_width() / 2);
                        interface_fill_polygons = to_polygons(closing_ex(
                            interface_fill_polygons, closing_distance));
                    }
                    if (organic_tree && !interface_as_base &&
                        &layer_ex == &interface_layer && !contact_keepout.empty()) {
                        interface_fill_polygons = diff(
                            std::move(interface_fill_polygons), contact_keepout);
                    }
                    const ExtrusionRole interface_role = interface_as_base ?
                        ExtrusionRole::erSupportMaterial : ExtrusionRole::erSupportMaterialInterface;
                    ExPolygons fill_regions = union_safety_offset_ex(std::move(interface_fill_polygons));
                    const int wall_count = raft_contact || interface_as_base ? 0 :
                        interface_layer_type == InterfaceLayerType::TopContact ? support_params.top_contact_wall_count :
                        interface_layer_type == InterfaceLayerType::BottomContact ? support_params.bottom_contact_wall_count :
                        bottom_interface ? support_params.bottom_interface_wall_count : support_params.top_interface_wall_count;
                    FillParams fill_params;
                    fill_params.density = density;
                    fill_params.dont_adjust = true;
                    fill_params.fill_concentric_gaps = fill_concentric_contact_gaps;
                    fill_params.preserve_short_paths = organic_tree && !interface_as_base;
                    fill_params.connect_concentric_loops = organic_tree && !interface_as_base && fill_pattern == ipConcentric;
                    if (organic_tree && !raft_contact && !interface_as_base && fill_pattern == ipRectilinear) {
                        // Generate the rows without unlimited contour anchors,
                        // then use the native bounded support connector. A long
                        // anchor beside a single narrow row would add another
                        // almost coincident road instead of preserving density.
                        fill_params.connect_support_zigzag = true;
                        fill_params.anchor_length = 0.f;
                        fill_params.anchor_length_max = 0.f;
                    }
                    if (fill_concentric_contact_gaps)
                        fill_params.flow = interface_flow;
                    ExPolygons wall_regions;
                    if (organic_tree && interface_layer_type == InterfaceLayerType::BottomContact &&
                        wall_count > 0 && layer_ex.layer->contact_polygons)
                        wall_regions = union_safety_offset_ex(*layer_ex.layer->contact_polygons);
                    if (!wall_regions.empty())
                        support_layer.support_islands = union_ex(support_layer.support_islands, wall_regions);
                    fill_support_interface_with_walls(layer_ex.extrusions, fill_regions, filler, fill_params,
                        interface_role, interface_flow, wall_count,
                        // Replacing the sparse structural planner must not
                        // also enable new under-width medial fragments. Keep
                        // the existing narrow-contact rescue policy separate
                        // from the choice of row-connection algorithm.
                        organic_tree && !interface_as_base &&
                            (legacy_fill_pattern == ipConcentric || legacy_fill_pattern == ipRectilinear),
                        wall_regions.empty() ? nullptr : &wall_regions);
                    if (organic_tree && (interface_layer_type == InterfaceLayerType::TopContact ||
                                         interface_layer_type == InterfaceLayerType::BottomContact)) {
                        Polygons printed;
                        for (const ExtrusionEntity *entity : layer_ex.extrusions)
                            if (entity != nullptr) entity->polygons_covered_by_width(printed, 1.f);
                        for (const ExPolygon &part : fill_regions)
                            if (area(intersection(printed, to_polygons(part))) > sqr(double(scale_(0.001))))
                                polygons_append(contact_keepout, to_polygons(part));
                    }
                    // Only generic roofs use compact source arcs. A true
                    // top/bottom contact pad and a model-landing interface
                    // retain their original footprint for the trunk handoff.
                    // Run after narrow rescue so every new roof path is checked.
                    if (organic_tree && interface_layer_type == InterfaceLayerType::Interface &&
                        layer_ex.layer->layer_type == SupporLayerType::TopInterface &&
                        layer_ex.layer->bottom_z > EPSILON)
                        collapse_narrow_interface_contours(layer_ex.extrusions, 0, fill_regions, interface_flow, true);
                }
            };
            extrude_interface(top_contact_layer,    raft_layer ? InterfaceLayerType::RaftContact : top_interfaces ? InterfaceLayerType::TopContact : InterfaceLayerType::InterfaceAsBase);
            extrude_interface(bottom_contact_layer, bottom_interfaces ? InterfaceLayerType::BottomContact : InterfaceLayerType::InterfaceAsBase);
            if (organic_tree && !interface_layer.empty()) {
                // Contact paths must be generated before they are used as a
                // keepout. Include both contact directions because an
                // interface layer can be shared by top and bottom contact
                // projections on the same output Z.
                for (const SupportGeneratorLayerExtruded *contact_layer : { &top_contact_layer, &bottom_contact_layer })
                    for (const ExtrusionEntity *entity : contact_layer->extrusions)
                        if (entity != nullptr)
                            entity->polygons_covered_by_width(contact_keepout, 1.f);
                if (!contact_keepout.empty())
                    contact_keepout = union_(std::move(contact_keepout));
            }
            const bool interface_layer_enabled = !interface_layer.empty() &&
                (interface_layer.layer->layer_type == SupporLayerType::BottomInterface ? bottom_interfaces : top_interfaces);
            extrude_interface(interface_layer,      interface_layer_enabled ? InterfaceLayerType::Interface : InterfaceLayerType::InterfaceAsBase);
            // Base interface layers under soluble interfaces
            if ( ! base_interface_layer.empty() && ! base_interface_layer.polygons_to_extrude().empty()) {
                Fill *filler = filler_base_interface.get();
                //FIXME Bottom interfaces are extruded with the briding flow. Some bridging layers have its height slightly reduced, therefore
                // the bridging flow does not quite apply. Reduce the flow to area of an ellipse? (A = pi * a * b)
                assert(! base_interface_layer.layer->bridging);
                Flow interface_flow = support_params.support_material_flow.with_height(float(base_interface_layer.layer->height));
                filler->angle   = support_interface_angle;
                filler->spacing = support_params.support_material_interface_flow.spacing();
                filler->link_max_length = coord_t(scale_(filler->spacing * link_max_length_factor / base_interface_density));
                fill_expolygons_generate_paths(
                    // Destination
                    base_interface_layer.extrusions,
                    //base_layer_interface.extrusions,
                    // Regions to fill
                    union_safety_offset_ex(base_interface_layer.polygons_to_extrude()),
                    // Filler and its parameters
                    filler, float(base_interface_density),
                    // Extrusion parameters
                    ExtrusionRole::erSupportMaterial, interface_flow);
            }

            // Base support or flange.
            if (! base_layer.empty() && ! base_layer.polygons_to_extrude().empty()) {
                Fill             *filler          = filler_support.get();
                filler->angle = angles[support_layer_id % angles.size()];
                // We don't use $base_flow->spacing because we need a constant spacing
                // value that guarantees that all layers are correctly aligned.
                assert(! base_layer.layer->bridging);
                auto flow = support_params.support_material_flow.with_height(float(base_layer.layer->height));
                filler->spacing = support_params.support_material_flow.spacing();
                filler->link_max_length = coord_t(scale_(filler->spacing * link_max_length_factor / support_params.support_density));
                float density = float(support_params.support_density);
                bool  sheath  = support_params.with_sheath;
                bool  no_sort = false;
                bool  done    = false;
                // Belt printers have no flat bed first layer — the belt is the tilted
                // build surface — so the dense raft_first_layer_density flange must not
                // fire anywhere, including the layer at z=0 (the belt-surface line).
                // (belt_floor_shear_factor is non-zero only when belt_printer is on.)
                // For every other printer type, support z is never negative, so this
                // matches the original "first layer at z=0" behaviour unchanged.
                const bool is_belt_printer = std::abs(slicing_params.belt_floor_shear_factor) > EPSILON;
                if (! is_belt_printer && base_layer.layer->bottom_z < EPSILON) {
                    // Base flange (the 1st layer).
                    filler = filler_first_layer;
                    filler->angle = Geometry::deg2rad(float(config.support_angle.value + 90.));
                    density = float(config.raft_first_layer_density.value * 0.01);
                    flow = support_params.first_layer_flow;
                    // use the proper spacing for first layer as we don't need to align
                    // its pattern to the other layers
                    //FIXME When paralellizing, each thread shall have its own copy of the fillers.
                    filler->spacing = flow.spacing();
                    filler->link_max_length = coord_t(scale_(filler->spacing * link_max_length_factor / density));
                    sheath  = true;
                    no_sort = true;
                } else if (support_params.support_style == SupportMaterialStyle::smsTreeOrganic &&
                           (config.support_base_pattern == smpNone || config.support_base_pattern == smpDefault)) {
                    tree_supports_generate_paths(base_layer.extrusions, base_layer.polygons_to_extrude(), flow, support_params);
                    done = true;
                }
                if (! done)
                    fill_expolygons_with_sheath_generate_paths(
                        // Destination
                        base_layer.extrusions,
                        // Regions to fill
                        base_layer.polygons_to_extrude(),
                        // Filler and its parameters
                        filler, density,
                        // Extrusion parameters
                        ExtrusionRole::erSupportMaterial, flow,
                        support_params, sheath, no_sort, false,
                        support_body_uses_zigzag_connections(
                            config.support_base_pattern, density, is_tree(config.support_type)),
                        !is_tree(config.support_type));
            }

            // Merge base_interface_layers to base_layers to avoid unneccessary retractions
            if (! base_layer.empty() && ! base_interface_layer.empty() && ! base_layer.polygons_to_extrude().empty() && ! base_interface_layer.polygons_to_extrude().empty() &&
                base_layer.could_merge(base_interface_layer))
                base_layer.merge(std::move(base_interface_layer));

            layer_cache.add_nonempty_and_sort();

            // Collect the support areas with this print_z into islands, as there is no need
            // for retraction over these islands.
            Polygons polys;
            // Collect the extrusions, sorted by the bottom extrusion height.
            for (LayerCacheItem &layer_cache_item : layer_cache.nonempty) {
                // Collect islands to polys.
                layer_cache_item.layer_extruded->polygons_append(polys);
                // The print_z of the top contact surfaces and bottom_z of the bottom contact surfaces are "free"
                // in a sense that they are not synchronized with other support layers. As the top and bottom contact surfaces
                // are inflated to achieve a better anchoring, it may happen, that these surfaces will at least partially
                // overlap in Z with another support layers, leading to over-extrusion.
                // Mitigate the over-extrusion by modulating the extrusion rate over these regions.
                // The print head will follow the same print_z, but the layer thickness will be reduced
                // where it overlaps with another support layer.
                //FIXME When printing a briging path, what is an equivalent height of the squished extrudate of the same width?
                // Collect overlapping top/bottom surfaces.
                layer_cache_item.overlapping.reserve(20);
                coordf_t bottom_z = layer_cache_item.layer_extruded->layer->bottom_print_z() + EPSILON;
                auto add_overlapping = [&layer_cache_item, bottom_z](const SupportGeneratorLayersPtr &layers, size_t idx_top) {
                    for (int i = int(idx_top) - 1; i >= 0 && layers[i]->print_z > bottom_z; -- i)
                        layer_cache_item.overlapping.push_back(layers[i]);
                };
                add_overlapping(top_contacts, idx_layer_top_contact);
                // An independent organic root may step down to several model
                // surfaces. Its next body/interface extrusion must start on
                // the highest contact beneath that road, not inside it. The
                // contact itself retains its model-derived bottom Z gap.
                if (support_params.support_style == smsTreeOrganic &&
                    layer_cache_item.layer_extruded->layer->layer_type != SupporLayerType::BottomContact)
                    add_overlapping(bottom_contacts, idx_layer_bottom_contact);
                if (support_params.support_style == smsTreeOrganic ||
                    layer_cache_item.layer_extruded->layer->layer_type == SupporLayerType::BottomContact) {
                    // A variable-height organic root may also overlap an
                    // existing trunk whose preceding plane is slightly higher.
                    // Preserve that trunk's flow height independently of the root.
                    add_overlapping(intermediate_layers,   idx_layer_intermediate);
                    add_overlapping(interface_layers,      idx_layer_interface);
                    add_overlapping(base_interface_layers, idx_layer_base_interface);
                }
                // Order the layers by lexicographically by an increasing print_z and a decreasing layer height.
                std::stable_sort(layer_cache_item.overlapping.begin(), layer_cache_item.overlapping.end(), [](auto *l1, auto *l2) { return *l1 < *l2; });
            }
            assert(support_layer.support_islands.empty());
            if (! polys.empty()) {
                support_layer.support_islands = union_ex(polys);
                // support_layer.support_islands_bboxes.reserve(support_layer.support_islands.size());
                // for (const ExPolygon &expoly : support_layer.support_islands)
                //     support_layer.support_islands_bboxes.emplace_back(get_extents(expoly).inflated(SCALED_EPSILON));
            }
        } // for each support_layer_id
    });

    // Track the original role before contacts/interfaces are flattened into
    // one support collection; an interface role alone cannot identify contacts.
    std::vector<std::set<const ExtrusionEntity *>> generic_interfaces(support_layers.size());
    // Now modulate the support layer height in parallel.
    tbb::parallel_for(tbb::blocked_range<size_t>(n_raft_layers, support_layers.size()),
        [&support_layers, &layer_caches, &support_params, &bbox_object, &generic_interfaces]
            (const tbb::blocked_range<size_t>& range) {
        for (size_t support_layer_id = range.begin(); support_layer_id < range.end(); ++ support_layer_id) {
            SupportLayer &support_layer = *support_layers[support_layer_id];
            LayerCache   &layer_cache   = layer_caches[support_layer_id];
            // For all extrusion types at this print_z, ordered by decreasing layer height:
            for (LayerCacheItem &layer_cache_item : layer_cache.nonempty) {
                // Trim the extrusion height from the bottom by the overlapping layers.
                modulate_extrusion_by_overlapping_layers(layer_cache_item.layer_extruded->extrusions, *layer_cache_item.layer_extruded->layer, layer_cache_item.overlapping);
                if (support_params.support_style == smsTreeOrganic && !layer_cache_item.layer_extruded->layer->bridging)
                    remove_unprintable_interface_paths(layer_cache_item.layer_extruded->extrusions);
                const SupporLayerType type = layer_cache_item.layer_extruded->layer->layer_type;
                if (type == SupporLayerType::TopInterface || type == SupporLayerType::BottomInterface)
                    generic_interfaces[support_layer_id].insert(layer_cache_item.layer_extruded->extrusions.begin(),
                        layer_cache_item.layer_extruded->extrusions.end());
                support_layer.support_fills.append(std::move(layer_cache_item.layer_extruded->extrusions));
            }

            // Inlong: Generate iron toolpath for contact layer
            if (!layer_cache.polys_to_iron.empty()) {
                auto f = std::unique_ptr<Fill>(Fill::new_from_type(support_params.ironing_pattern));
                f->set_bounding_box(bbox_object);
                f->layer_id        = support_layer.id();
                f->z               = support_layer.print_z;
                f->overlap         = 0;
                f->angle           = layer_cache.ironing_angle;
                f->spacing         = support_params.ironing_spacing;
                f->link_max_length = (coord_t) scale_(3. * f->spacing);

                ExPolygons polys_to_iron = union_safety_offset_ex(layer_cache.polys_to_iron);
                layer_cache.polys_to_iron.clear();

                // Find the layer above that directly overlaps current layer, clip the overlapped part
                if (support_layer_id < support_layers.size() - 1) {
                    const auto& upper_layer = support_layers[support_layer_id + 1];
                    if (!upper_layer->support_islands.empty() && upper_layer->bottom_z() <= support_layer.print_z + EPSILON) {
                        polys_to_iron = diff_ex(polys_to_iron, upper_layer->support_islands);
                    }
                }

                fill_expolygons_generate_paths(
                    // Destination
                    support_layer.support_fills.entities,
                    // Regions to fill
                    std::move(polys_to_iron),
                    // Filler and its parameters
                    f.get(), 1.f,
                    // Extrusion parameters
                    ExtrusionRole::erIroning, support_params.ironing_flow);
            }
        }
    });

    if (support_params.support_style == smsTreeOrganic)
        remove_unused_compact_interfaces(support_layers, generic_interfaces,
            float(support_params.support_material_interface_flow.width()));

    if (support_params.support_style == smsTreeOrganic &&
        std::any_of(bottom_contacts.begin(), bottom_contacts.end(), [](const SupportGeneratorLayer *contact) {
            return contact != nullptr && contact->contact_polygons != nullptr;
        })) {
        // A restored stepped rim extends beyond the true landing patches.
        // Source-polygon overlap tests therefore cannot describe every bead
        // it supports. Normalize all affected physical support slabs against
        // already printed material, including the later trunk/interface roads.
        std::set<const ExtrusionEntity *> support_owners;
        std::function<void(const ExtrusionEntityCollection &)> collect = [&](const ExtrusionEntityCollection &group) {
            for (const ExtrusionEntity *entity : group.entities)
                if (const auto *children = dynamic_cast<const ExtrusionEntityCollection *>(entity)) collect(*children);
                else if (is_support(entity->role())) support_owners.insert(entity);
        };
        for (const SupportLayer *layer : support_layers) collect(layer->support_fills);
        physical_height_adjustments = modulate_joined_support_paths(
            support_layers, float(slicing_params.min_layer_height), support_owners);
    }

#ifndef NDEBUG
    struct Test {
        static bool verify_nonempty(const ExtrusionEntityCollection *collection) {
            for (const ExtrusionEntity *ee : collection->entities) {
                if (const ExtrusionPath *path = dynamic_cast<const ExtrusionPath*>(ee))
                    assert(! path->empty());
                else if (const ExtrusionMultiPath *multipath = dynamic_cast<const ExtrusionMultiPath*>(ee))
                    assert(! multipath->empty());
                else if (const ExtrusionEntityCollection *eecol = dynamic_cast<const ExtrusionEntityCollection*>(ee)) {
                    assert(! eecol->empty());
                    return verify_nonempty(eecol);
                } else
                    assert(false);
            }
            return true;
        }
    };
    for (const SupportLayer *support_layer : support_layers)
        assert(Test::verify_nonempty(&support_layer->support_fills));
#endif // NDEBUG
    return physical_height_adjustments;
}

/*
void PrintObjectSupportMaterial::clip_by_pillars(
    const PrintObject   &object,
    LayersPtr           &bottom_contacts,
    LayersPtr           &top_contacts,
    LayersPtr           &intermediate_contacts);

{
    // this prevents supplying an empty point set to BoundingBox constructor
    if (top_contacts.empty())
        return;

    coord_t pillar_size    = scale_(PILLAR_SIZE);
    coord_t pillar_spacing = scale_(PILLAR_SPACING);

    // A regular grid of pillars, filling the 2D bounding box.
    Polygons grid;
    {
        // Rectangle with a side of 2.5x2.5mm.
        Polygon pillar;
        pillar.points.push_back(Point(0, 0));
        pillar.points.push_back(Point(pillar_size, 0));
        pillar.points.push_back(Point(pillar_size, pillar_size));
        pillar.points.push_back(Point(0, pillar_size));

        // 2D bounding box of the projection of all contact polygons.
        BoundingBox bbox;
        for (LayersPtr::const_iterator it = top_contacts.begin(); it != top_contacts.end(); ++ it)
            bbox.merge(get_extents((*it)->polygons));
        grid.reserve(size_t(ceil(bb.size()(0) / pillar_spacing)) * size_t(ceil(bb.size()(1) / pillar_spacing)));
        for (coord_t x = bb.min(0); x <= bb.max(0) - pillar_size; x += pillar_spacing) {
            for (coord_t y = bb.min(1); y <= bb.max(1) - pillar_size; y += pillar_spacing) {
                grid.push_back(pillar);
                for (size_t i = 0; i < pillar.points.size(); ++ i)
                    grid.back().points[i].translate(Point(x, y));
            }
        }
    }

    // add pillars to every layer
    for my $i (0..n_support_z) {
        $shape->[$i] = [ @$grid ];
    }

    // build capitals
    for my $i (0..n_support_z) {
        my $z = $support_z->[$i];

        my $capitals = intersection(
            $grid,
            $contact->{$z} // [],
        );

        // work on one pillar at time (if any) to prevent the capitals from being merged
        // but store the contact area supported by the capital because we need to make
        // sure nothing is left
        my $contact_supported_by_capitals = [];
        foreach my $capital (@$capitals) {
            // enlarge capital tops
            $capital = offset([$capital], +($pillar_spacing - $pillar_size)/2);
            push @$contact_supported_by_capitals, @$capital;

            for (my $j = $i-1; $j >= 0; $j--) {
                my $jz = $support_z->[$j];
                $capital = offset($capital, -$self->interface_flow->scaled_width/2);
                last if !@$capitals;
                push @{ $shape->[$j] }, @$capital;
            }
        }

        // Capitals will not generally cover the whole contact area because there will be
        // remainders. For now we handle this situation by projecting such unsupported
        // areas to the ground, just like we would do with a normal support.
        my $contact_not_supported_by_capitals = diff(
            $contact->{$z} // [],
            $contact_supported_by_capitals,
        );
        if (@$contact_not_supported_by_capitals) {
            for (my $j = $i-1; $j >= 0; $j--) {
                push @{ $shape->[$j] }, @$contact_not_supported_by_capitals;
            }
        }
    }
}

sub clip_with_shape {
    my ($self, $support, $shape) = @_;

    foreach my $i (keys %$support) {
        // don't clip bottom layer with shape so that we
        // can generate a continuous base flange
        // also don't clip raft layers
        next if $i == 0;
        next if $i < $self->object_config->raft_layers;
        $support->{$i} = intersection(
            $support->{$i},
            $shape->[$i],
        );
    }
}
*/

Vec2d build_plate_tilt_slope(const PrintConfig &print_config)
{
    auto slope = [](double tilt_deg) { return std::tan(Geometry::deg2rad(std::clamp(tilt_deg, -89., 89.))); };
    return { slope(print_config.build_plate_tilt_y.value), slope(print_config.build_plate_tilt_x.value) };
}

} // namespace Slic3r
