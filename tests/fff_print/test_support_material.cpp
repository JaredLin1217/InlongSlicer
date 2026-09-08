#include <catch2/catch_all.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <tbb/global_control.h>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCode/ConflictChecker.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Support/SupportCommon.hpp"
#include "libslic3r/Support/TreeSupport.hpp"
#include "libslic3r/Support/TreeSupportUtils.hpp"
#include "libslic3r/Thread.hpp"

#include <cmath>
#include <set>
#include <vector>

#include "test_helpers.hpp" // get access to init_print, etc
#include "test_utils.hpp"

// Not self-contained: its inline constructor uses PrintObject, PrintRegion, SlicingParameters and
// Geometry, so it must follow the headers (pulled in via test_helpers.hpp) that define them.
#include "libslic3r/Support/SupportParameters.hpp"

using namespace Slic3r::Test;
using namespace Slic3r;

namespace {

DynamicPrintConfig organic_support_config(
    bool independent_layers,
    bool independent_top_contact,
    double object_layer_height,
    double top_z_distance,
    double min_layer_height = 0.05,
    double max_layer_height = 0.4)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", true },
        { "support_type", "tree(auto)" },
        { "support_style", "organic" },
        { "support_threshold_angle", 30 },
        { "dont_support_bridges", false },
        { "layer_height", object_layer_height },
        { "initial_layer_print_height", object_layer_height },
        { "nozzle_diameter", "0.6" },
        { "min_layer_height", min_layer_height },
        { "max_layer_height", max_layer_height },
        { "support_top_z_distance", top_z_distance },
        { "support_bottom_z_distance", 0.2 },
        { "support_interface_top_layers", 2 },
        { "independent_support_layer_height", independent_layers },
        { "independent_support_top_contact_layer_height", independent_top_contact }
    });
    return config;
}

std::vector<const SupportLayer *> nonempty_support_layers(const Print &print)
{
    std::vector<const SupportLayer *> result;
    for (const SupportLayer *layer : print.objects().front()->support_layers())
        if (layer != nullptr && layer->has_extrusions())
            result.push_back(layer);
    return result;
}

bool is_on_regular_grid(double z, double layer_height)
{
    return std::abs(z / layer_height - std::round(z / layer_height)) <= 1e-4;
}

ExPolygon rectangular_area(double min_x, double min_y, double max_x, double max_y)
{
    ExPolygon area;
    area.contour.points = {
        Point::new_scale(min_x, min_y), Point::new_scale(max_x, min_y),
        Point::new_scale(max_x, max_y), Point::new_scale(min_x, max_y)};
    area.contour.make_counter_clockwise();
    return area;
}

bool areas_contain(const ExPolygons &areas, double x, double y)
{
    const Point point = Point::new_scale(x, y);
    return std::any_of(areas.begin(), areas.end(), [&point](const ExPolygon &area) {
        return area.contains(point);
    });
}

DynamicPrintConfig raft_support_config(const std::string &style, bool tree_support, int raft_layers)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_support", true},
        {"support_type", tree_support ? "tree(auto)" : "normal(auto)"},
        {"support_style", style},
        {"support_threshold_angle", 30},
        {"dont_support_bridges", false},
        {"support_on_build_plate_only", false},
        {"layer_height", 0.2},
        {"initial_layer_print_height", 0.2},
        {"nozzle_diameter", "0.6"},
        {"min_layer_height", "0.05"},
        {"max_layer_height", "0.45"},
        {"raft_layers", raft_layers},
        {"raft_first_layer_density", 100},
        {"raft_expansion", 1.0},
        {"raft_first_layer_expansion", 1.0},
        {"independent_support_layer_height", false},
        {"independent_support_top_contact_layer_height", false}
    });
    return config;
}

size_t extruded_raft_layer_count(const Print &print)
{
    const PrintObject *object = print.objects().front();
    const coordf_t raft_top_z = object->slicing_parameters().raft_contact_top_z;
    return std::count_if(object->support_layers().begin(), object->support_layers().end(),
        [raft_top_z](const SupportLayer *layer) {
            return layer != nullptr && layer->has_extrusions() && layer->print_z <= raft_top_z + EPSILON;
        });
}

double raft_volume(TestMesh mesh, const DynamicPrintConfig &config)
{
    Print print;
    init_and_process_print({mesh}, print, config);
    const coordf_t raft_top_z = print.objects().front()->slicing_parameters().raft_contact_top_z;
    double volume = 0.;
    size_t layers = 0;
    for (const SupportLayer *layer : print.objects().front()->support_layers())
        if (layer != nullptr && layer->has_extrusions() && layer->print_z <= raft_top_z + EPSILON) {
            volume += layer->support_fills.total_volume();
            ++layers;
        }
    if (layers == 0)
        throw std::runtime_error("No extruded raft layer was generated");
    return volume;
}

ExtrusionEntityCollection horizontal_raft_path(double min_x, double max_x, double y)
{
    ExtrusionPath path(erSupportMaterial, 0.08, 0.4f, 0.2f);
    path.polyline = Polyline3(Polyline{
        Point::new_scale(min_x, y),
        Point::new_scale(max_x, y)});
    ExtrusionEntityCollection paths;
    paths.append(std::move(path));
    return paths;
}

double first_layer_extrusion_amount(const std::string &gcode, double first_layer_z)
{
    double amount = 0.;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&amount, first_layer_z](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.extruding(self) && self.z() <= first_layer_z + EPSILON)
            amount += line.dist_E(self);
    });
    return amount;
}

double two_object_box_raft_first_layer_extrusion(
    double center_distance,
    bool   shared_object = false,
    double raft_expansion = 10.0,
    double layer_expansion_step = 0.0,
    int    raft_layers = 2)
{
    DynamicPrintConfig config = raft_support_config("grid", false, raft_layers);
    config.set_deserialize_strict({
        {"enable_support", false},
        {"raft_generate_bounding_box", true},
        {"raft_ignore_internal_contours", false},
        {"raft_expansion", raft_expansion},
        {"raft_layer_expansion_step", layer_expansion_step},
        {"raft_first_layer_expansion", 0.0},
        {"skirt_loops", 0},
        {"machine_start_gcode", ""},
        {"machine_end_gcode", ""}
    });

    Model model;
    Print print;
    if (shared_object) {
        init_print({TestMesh::cube_20x20x20}, print, model, config);
        ModelObject *object = model.objects.front();
        object->name = "shared_raft_object.stl";
        object->instances.front()->set_offset(Vec3d(60., 60., 0.));
        object->add_instance()->set_offset(Vec3d(60. + center_distance, 60., 0.));
    } else {
        init_print({TestMesh::cube_20x20x20, TestMesh::cube_20x20x20}, print, model, config);
        for (size_t object_index = 0; object_index < model.objects.size(); ++object_index) {
            ModelObject *object = model.objects[object_index];
            object->name = "raft_object_" + std::to_string(object_index) + ".stl";
            object->instances.front()->set_offset(Vec3d(60. + object_index * center_distance, 60., 0.));
        }
    }
    const char *stage = "apply";
    try {
        print.apply(model, config);
        stage = "validate";
        print.validate();
        print.set_status_silent();
        stage = "process";
        print.process();
        if (print.get_conflict_result().has_value()) {
            std::ostringstream details;
            for (const PrintObject *object : print.objects()) {
                details << " object_raft_layers=" << object->slicing_parameters().raft_layers();
                for (const SupportLayer *layer : object->support_layers())
                    details << " support_id=" << layer->id() << "/z=" << layer->print_z;
            }
            throw std::runtime_error("Overlapping raft paths were reported as an object conflict:" + details.str());
        }
        ScopedTemporaryFile output(".gcode");
        stage = "export";
        print.export_gcode(output.string(), nullptr, nullptr);
        stage = "read";
        std::ifstream stream(output.string());
        const std::string output_gcode(
            (std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        return first_layer_extrusion_amount(output_gcode, config.opt_float("initial_layer_print_height"));
    } catch (const std::exception &error) {
        throw std::runtime_error(
            "Two-object raft G-code failed at center distance " + std::to_string(center_distance) +
            " during " + stage + ": " + error.what());
    }
}

std::vector<double> raft_layer_widths(TestMesh mesh, const DynamicPrintConfig &config)
{
    Model model;
    Print print;
    init_print({mesh}, print, model, config);
    model.objects.front()->instances.front()->set_offset(Vec3d(60., 60., 0.));
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    const PrintObject *object = print.objects().front();
    const size_t raft_layers = object->slicing_parameters().raft_layers();
    std::vector<double> widths;
    for (const SupportLayer *layer : object->support_layers()) {
        if (layer == nullptr || layer->id() >= raft_layers || !layer->has_extrusions())
            continue;

        ExPolygons footprint = union_ex(layer->support_fills.polygons_covered_by_spacing());
        if (footprint.empty()) {
            footprint = layer->raft_support_islands;
            expolygons_append(footprint, layer->raft_interface_islands);
        }
        if (footprint.empty())
            footprint = layer->support_islands;
        const BoundingBox bbox = get_extents(footprint);
        if (!bbox.defined)
            throw std::runtime_error("Raft layer has no measurable footprint");
        widths.emplace_back(unscale<double>(bbox.size().x()));
    }
    return widths;
}

void process_overlapping_box_rafts(
    Print &print, Model &model, bool shared_object,
    const std::string &style = "grid", bool tree_support = false, int raft_layers = 2)
{
    DynamicPrintConfig config = raft_support_config(style, tree_support, raft_layers);
    config.set_deserialize_strict({
        {"enable_support", false},
        {"raft_generate_bounding_box", true},
        {"raft_ignore_internal_contours", false},
        {"raft_expansion", 10.0},
        {"raft_first_layer_expansion", 0.0},
        {"skirt_loops", 0}
    });

    if (shared_object) {
        init_print({TestMesh::cube_20x20x20}, print, model, config);
        ModelObject *object = model.objects.front();
        object->instances.front()->set_offset(Vec3d(60., 60., 0.));
        for (size_t index = 1; index < 3; ++index)
            object->add_instance()->set_offset(Vec3d(60. + 25. * index, 60., 0.));
    } else {
        init_print(
            {TestMesh::cube_20x20x20, TestMesh::cube_20x20x20, TestMesh::cube_20x20x20},
            print, model, config);
        for (size_t index = 0; index < model.objects.size(); ++index)
            model.objects[index]->instances.front()->set_offset(Vec3d(60. + 25. * index, 60., 0.));
    }

    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();
}

using TreeNodeSignatureEntry = std::array<int64_t, 11>;
using TreeContactSignatureEntry = std::array<int64_t, 4>;

struct TreeSupportMetrics
{
    std::vector<TreeNodeSignatureEntry> node_signature;
    std::vector<TreeContactSignatureEntry> contact_signature;
    std::vector<double> free_segment_angles;
    std::map<int64_t, size_t> free_node_counts;
    size_t buildplate_nodes = 0;
};

TriangleMesh suspended_tree_platform()
{
    TriangleMesh platform = make_cube(36., 12., 2.);
    platform.translate(0., 0., 20.);

    // Keep the disconnected platform elevated when the test helper drops the object onto the bed.
    // The anchor is far enough away that the support below the platform remains unobstructed.
    TriangleMesh anchor = make_cube(1., 1., 1.);
    anchor.translate(50., 0., 0.);
    platform.merge(anchor);
    return platform;
}

DynamicPrintConfig classic_tree_support_config(
    const std::string &style, double maximum_angle, double preferred_angle)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"enable_support", true},
        {"support_type", "tree(auto)"},
        {"support_style", style},
        {"support_threshold_angle", 30},
        {"dont_support_bridges", false},
        {"max_bridge_length", 0},
        {"support_on_build_plate_only", true},
        {"support_remove_small_overhang", false},
        {"layer_height", 0.2},
        {"initial_layer_print_height", 0.2},
        {"nozzle_diameter", "0.6"},
        {"min_layer_height", "0.05"},
        {"max_layer_height", "0.4"},
        {"support_line_width", "0.6"},
        {"support_top_z_distance", 0.2},
        {"support_bottom_z_distance", 0.2},
        {"support_interface_top_layers", 2},
        {"support_interface_bottom_layers", 0},
        {"support_top_contact_spacing", 0.2},
        {"support_interface_spacing", 0.5},
        {"support_base_pattern_spacing", 2.5},
        {"tree_support_branch_angle", maximum_angle},
        {"tree_support_angle_slow", preferred_angle},
        {"tree_support_branch_distance", 5.0},
        {"tree_support_branch_diameter", 2.0},
        {"tree_support_wall_count", 1},
        {"tree_support_auto_brim", false},
        {"tree_support_brim_width", 0.0},
        {"independent_support_layer_height", false},
        {"independent_support_top_contact_layer_height", false},
        {"skirt_loops", 0}
    });
    return config;
}

TreeSupportMetrics tree_support_metrics(Print &print)
{
    TreeSupportMetrics result;
    PrintObject *object = print.get_object(0);
    const std::shared_ptr<TreeSupportData> tree_data = object->alloc_tree_support_preview_cache();

    for (const std::unique_ptr<SupportNode> &owned_node : tree_data->contact_nodes) {
        const SupportNode &node = *owned_node;
        const SupportNode *parent = node.parent;
        const int64_t z = std::llround(node.print_z * 1'000'000.);
        const int64_t parent_z = parent == nullptr ? 0 : std::llround(parent->print_z * 1'000'000.);
        result.node_signature.push_back({
            int64_t(node.position.x()), int64_t(node.position.y()), z,
            parent == nullptr ? 0 : 1,
            parent == nullptr ? 0 : int64_t(parent->position.x()),
            parent == nullptr ? 0 : int64_t(parent->position.y()),
            parent_z,
            node.valid ? 1 : 0,
            node.to_buildplate ? 1 : 0,
            int64_t(node.distance_to_top),
            int64_t(node.obj_layer_nr)
        });

        if (parent == nullptr && node.distance_to_top <= 0)
            result.contact_signature.push_back({
                int64_t(node.position.x()), int64_t(node.position.y()), z, int64_t(node.obj_layer_nr)});

        if (node.valid && node.to_buildplate && node.print_z <= 0.2 + EPSILON)
            ++result.buildplate_nodes;

        constexpr double free_min_z = 5.5;
        constexpr double free_max_z = 18.0;
        if (node.valid && node.to_buildplate &&
            node.print_z >= free_min_z && node.print_z <= free_max_z)
            ++result.free_node_counts[std::llround(node.print_z * 1'000.)];

        if (parent == nullptr || !node.to_buildplate || !parent->to_buildplate ||
            node.print_z < free_min_z || parent->print_z > free_max_z)
            continue;

        const double vertical_distance = parent->print_z - node.print_z;
        if (vertical_distance <= EPSILON)
            continue;
        const double dx = unscale<double>(parent->position.x() - node.position.x());
        const double dy = unscale<double>(parent->position.y() - node.position.y());
        const double horizontal_distance = std::hypot(dx, dy);
        result.free_segment_angles.push_back(
            std::atan2(horizontal_distance, vertical_distance) * 180. / M_PI);
    }

    std::sort(result.node_signature.begin(), result.node_signature.end());
    std::sort(result.contact_signature.begin(), result.contact_signature.end());

    return result;
}

TreeSupportMetrics process_tree_support(
    const std::string &style, double maximum_angle, double preferred_angle)
{
    // Initialize the worker pool before restricting this test slice to one worker.
    // Initializing it under the restriction deadlocks the worker-naming barrier.
    name_tbb_thread_pool_threads_set_locale();
    const tbb::global_control single_thread(
        tbb::global_control::max_allowed_parallelism, 1);
    Print print;
    init_and_process_print(
        {suspended_tree_platform()}, print,
        classic_tree_support_config(style, maximum_angle, preferred_angle));
    return tree_support_metrics(print);
}

} // namespace

TEST_CASE("Strong tree honors the preferred angle in unobstructed space", "[TreeSupport][StrongTree]")
{
    const TreeSupportMetrics preferred_10 = process_tree_support("tree_strong", 30., 10.);
    const TreeSupportMetrics preferred_25 = process_tree_support("tree_strong", 30., 25.);

    REQUIRE(preferred_10.buildplate_nodes > 0);
    REQUIRE(preferred_25.buildplate_nodes > 0);
    REQUIRE_FALSE(preferred_10.free_segment_angles.empty());
    REQUIRE_FALSE(preferred_25.free_segment_angles.empty());

    constexpr double angle_tolerance = 0.75;
    for (double angle : preferred_10.free_segment_angles)
        REQUIRE(angle <= 10. + angle_tolerance);
    for (double angle : preferred_25.free_segment_angles)
        REQUIRE(angle <= 25. + angle_tolerance);

    const double maximum_10 = *std::max_element(
        preferred_10.free_segment_angles.begin(), preferred_10.free_segment_angles.end());
    const double maximum_25 = *std::max_element(
        preferred_25.free_segment_angles.begin(), preferred_25.free_segment_angles.end());
    CAPTURE(maximum_10, maximum_25);
    REQUIRE(maximum_25 > maximum_10 + 2.);
    REQUIRE(maximum_25 > 12.);

    size_t compared_layers = 0;
    for (const auto &[z, low_count] : preferred_10.free_node_counts) {
        const auto high = preferred_25.free_node_counts.find(z);
        if (high == preferred_25.free_node_counts.end())
            continue;
        ++compared_layers;
        REQUIRE(low_count >= high->second);
    }
    REQUIRE(compared_layers > 0);

    REQUIRE(preferred_10.contact_signature == preferred_25.contact_signature);
}

TEST_CASE("Strong tree clamps the preferred angle to the maximum branch angle", "[TreeSupport][StrongTree]")
{
    const TreeSupportMetrics preferred_30 = process_tree_support("tree_strong", 30., 30.);
    const TreeSupportMetrics preferred_45 = process_tree_support("tree_strong", 30., 45.);

    REQUIRE(preferred_30.buildplate_nodes > 0);
    REQUIRE(preferred_45.buildplate_nodes > 0);
    REQUIRE(preferred_30.contact_signature == preferred_45.contact_signature);
    REQUIRE_FALSE(preferred_30.free_segment_angles.empty());
    REQUIRE_FALSE(preferred_45.free_segment_angles.empty());
    for (double angle : preferred_30.free_segment_angles)
        REQUIRE(angle <= 30.75);
    for (double angle : preferred_45.free_segment_angles) {
        REQUIRE(std::isfinite(angle));
        REQUIRE(angle <= 30.75);
    }
    REQUIRE(*std::max_element(
        preferred_30.free_segment_angles.begin(), preferred_30.free_segment_angles.end()) > 20.);
    REQUIRE(*std::max_element(
        preferred_45.free_segment_angles.begin(), preferred_45.free_segment_angles.end()) > 20.);

    const TreeSupportMetrics zero_maximum = process_tree_support("tree_strong", 0., 25.);
    REQUIRE(zero_maximum.buildplate_nodes > 0);
    REQUIRE_FALSE(zero_maximum.free_segment_angles.empty());
    for (double angle : zero_maximum.free_segment_angles) {
        REQUIRE(std::isfinite(angle));
        REQUIRE(angle <= 0.75);
    }
}

TEST_CASE("Preferred branch angle does not affect other classic tree styles", "[TreeSupport][StrongTree]")
{
    for (const std::string style : {"tree_slim", "tree_hybrid"}) {
        DYNAMIC_SECTION(style << " ignores the preferred branch angle") {
            const TreeSupportMetrics preferred_10 = process_tree_support(style, 30., 10.);
            const TreeSupportMetrics preferred_45 = process_tree_support(style, 30., 45.);

            REQUIRE(preferred_10.buildplate_nodes > 0);
            REQUIRE(preferred_45.buildplate_nodes > 0);
            REQUIRE(preferred_10.contact_signature == preferred_45.contact_signature);
            REQUIRE_FALSE(preferred_10.free_segment_angles.empty());
            REQUIRE_FALSE(preferred_45.free_segment_angles.empty());
            REQUIRE(*std::max_element(
                preferred_10.free_segment_angles.begin(), preferred_10.free_segment_angles.end()) > 15.);
            REQUIRE(*std::max_element(
                preferred_45.free_segment_angles.begin(), preferred_45.free_segment_angles.end()) > 15.);
        }
    }
}

TEST_CASE("Changing the strong tree preferred angle invalidates support once", "[TreeSupport][StrongTree]")
{
    Model model;
    Print print;
    DynamicPrintConfig preferred_10 = classic_tree_support_config("tree_strong", 30., 10.);
    init_print({suspended_tree_platform()}, print, model, preferred_10);
    print.process();

    REQUIRE(print.objects().front()->is_step_done(posSupportMaterial));
    const std::shared_ptr<TreeSupportData> first_cache =
        print.get_object(0)->alloc_tree_support_preview_cache();
    const std::vector<TreeNodeSignatureEntry> first_signature =
        tree_support_metrics(print).node_signature;

    DynamicPrintConfig preferred_25 = classic_tree_support_config("tree_strong", 30., 25.);
    const PrintBase::ApplyStatus changed_status = print.apply(model, preferred_25);
    REQUIRE(changed_status == PrintBase::APPLY_STATUS_INVALIDATED);
    REQUIRE_FALSE(print.objects().front()->is_step_done(posSupportMaterial));

    print.validate();
    print.process();
    REQUIRE(print.objects().front()->is_step_done(posSupportMaterial));
    const std::shared_ptr<TreeSupportData> second_cache =
        print.get_object(0)->alloc_tree_support_preview_cache();
    const std::vector<TreeNodeSignatureEntry> second_signature =
        tree_support_metrics(print).node_signature;
    REQUIRE(second_cache != first_cache);
    REQUIRE(second_signature != first_signature);

    const PrintBase::ApplyStatus unchanged_status = print.apply(model, preferred_25);
    REQUIRE(unchanged_status != PrintBase::APPLY_STATUS_INVALIDATED);
    REQUIRE(print.objects().front()->is_step_done(posSupportMaterial));
    REQUIRE(print.get_object(0)->alloc_tree_support_preview_cache() == second_cache);
}

TEST_CASE("Explicit rectilinear support uses alternating end connections", "[SupportMaterial][NormalSupport]")
{
    constexpr coordf_t sparse_density = 0.1;

    REQUIRE(support_body_fill_pattern(smpRectilinear, sparse_density, false, false) == ipRectilinear);
    REQUIRE(support_body_fill_pattern(smpDefault, sparse_density, false, false) == ipSupportBase);
    REQUIRE(support_body_fill_pattern(smpRectilinearGrid, sparse_density, false, false) == ipSupportBase);
    REQUIRE(support_body_fill_pattern(smpConcentric, sparse_density, false, false) == ipConcentric);
    REQUIRE(support_body_fill_pattern(smpHoneycomb, sparse_density, false, false) == ipHoneycomb);

    REQUIRE(support_body_uses_zigzag_connections(smpRectilinear, sparse_density, false));
    REQUIRE_FALSE(support_body_uses_zigzag_connections(smpRectilinear, 1., false));
    REQUIRE_FALSE(support_body_uses_zigzag_connections(smpDefault, sparse_density, false));

    SECTION("Rafts and tree supports retain their structural planner")
    {
        REQUIRE(support_base_fill_pattern(smpRectilinear, sparse_density, false) == ipSupportBase);
        REQUIRE(support_body_fill_pattern(smpRectilinear, sparse_density, false, true) == ipSupportBase);
        REQUIRE_FALSE(support_body_uses_zigzag_connections(smpRectilinear, sparse_density, true));
    }
}

TEST_CASE("Explicit rectilinear support uses native contour topology without an outline", "[SupportMaterial][NormalSupport]")
{
    const ExPolygon support_region{Polygon{Points{
        Point::new_scale(0., 0.),
        Point::new_scale(36., 0.),
        Point::new_scale(36., 24.),
        Point::new_scale(24., 24.),
        Point::new_scale(24., 14.),
        Point::new_scale(18., 14.),
        Point::new_scale(18., 24.),
        Point::new_scale(0., 24.)}}};
    const Flow flow(0.63f, 0.3f, 0.6f);

    auto fill_paths = [&](bool connect_support_zigzag) {
        std::unique_ptr<Fill> filler(Fill::new_from_type(ipRectilinear));
        filler->set_bounding_box(get_extents(support_region.contour));
        filler->angle = 0.;
        filler->layer_id = 1;
        filler->spacing = flow.spacing();

        FillParams fill_params;
        fill_params.density = 0.1f;
        fill_params.dont_adjust = true;
        fill_params.connect_support_zigzag = connect_support_zigzag;
        if (!connect_support_zigzag)
            fill_params.anchor_length_max = 0.f;

        const Surface surface(stInternal, support_region);
        return filler->fill_surface(&surface, fill_params);
    };
    const Polylines disconnected = fill_paths(false);
    const Polylines connected = fill_paths(true);
    constexpr double primary_min_length = 12.;
    auto is_primary_segment = [primary_min_length](const Point &first, const Point &second) {
        const Vec2d delta = (second - first).cast<double>();
        return std::abs(unscale_(delta.x())) <= 1e-3 &&
               std::abs(unscale_(delta.y())) > primary_min_length;
    };
    auto primary_segment_count = [&is_primary_segment](const Polylines &paths) {
        size_t count = 0;
        for (const Polyline &path : paths)
            for (size_t idx = 1; idx < path.points.size(); ++idx)
                if (is_primary_segment(path.points[idx - 1], path.points[idx]))
                    ++count;
        return count;
    };
    bool has_boundary_arc_connector = false;
    auto connector_lengths = [&is_primary_segment, &has_boundary_arc_connector](const Polylines &paths) {
        std::vector<double> lengths;
        for (const Polyline &path : paths) {
            bool seen_primary = false;
            double connector_length = 0.;
            size_t connector_segments = 0;
            for (size_t idx = 1; idx < path.points.size(); ++idx) {
                const Point &first = path.points[idx - 1];
                const Point &second = path.points[idx];
                if (is_primary_segment(first, second)) {
                    if (seen_primary && connector_length > 0.) {
                        lengths.emplace_back(connector_length);
                        has_boundary_arc_connector |= connector_segments > 1;
                    }
                    seen_primary = true;
                    connector_length = 0.;
                    connector_segments = 0;
                } else if (seen_primary) {
                    connector_length += unscale_((second - first).cast<double>().norm());
                    ++connector_segments;
                }
            }
        }
        return lengths;
    };
    const std::vector<double> connectors = connector_lengths(connected);
    const double line_spacing = flow.spacing() / 0.1;

    CAPTURE(disconnected.size(), connected.size(), connectors, has_boundary_arc_connector);
    REQUIRE_FALSE(disconnected.empty());
    REQUIRE_FALSE(connected.empty());
    REQUIRE(connected.size() < disconnected.size());
    REQUIRE(connected.size() == 1);
    const size_t primary_count = primary_segment_count(connected);
    REQUIRE(primary_count == primary_segment_count(disconnected));
    REQUIRE_FALSE(connectors.empty());
    REQUIRE(connectors.size() == primary_count - 1);
    REQUIRE(has_boundary_arc_connector);
    REQUIRE(std::all_of(connectors.begin(), connectors.end(), [line_spacing](double length) {
        return length <= 3. * line_spacing + 1e-3;
    }));
    REQUIRE(std::any_of(connectors.begin(), connectors.end(), [line_spacing](double length) {
        return length > 2.5 * line_spacing + 1e-3;
    }));
    REQUIRE(std::none_of(connected.begin(), connected.end(), [](const Polyline &path) {
        return path.points.size() > 2 && path.first_point() == path.last_point();
    }));
    REQUIRE(std::all_of(connected.begin(), connected.end(), [&support_region](const Polyline &path) {
        return support_region.contains(path);
    }));
    for (const Polyline &path : connected) {
        int previous_direction = 0;
        for (size_t idx = 1; idx < path.points.size(); ++idx) {
            if (!is_primary_segment(path.points[idx - 1], path.points[idx]))
                continue;
            const Vec2d delta = (path.points[idx] - path.points[idx - 1]).cast<double>();
            const int direction = delta.y() > 0. ? 1 : -1;
            if (previous_direction != 0)
                REQUIRE(direction == -previous_direction);
            previous_direction = direction;
        }
    }
}

// Distinct layer Z heights carrying support interface extrusion.
static size_t support_interface_layer_count(const std::string &gcode)
{
    return layers_with_role(gcode, "support material interface").size();
}

// Distinct layer Z heights carrying support base extrusion. The base G-code label "support material"
// is a substring of "support material interface", so a base line is a support line that is not an
// interface line.
static size_t support_base_layer_count(const std::string &gcode)
{
    std::set<double> layers;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&layers](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (! line.extruding(self)) return;
        const std::string_view comment = line.comment();
        if (comment.find("support material") != std::string_view::npos &&
            comment.find("interface") == std::string_view::npos)
            layers.insert(self.z());
    });
    return layers.size();
}

// Dominant support-interface fill direction per interface layer, in radians [0, pi). Uses the
// length-weighted axial mean (each segment angle doubled so a line and its reverse agree, then
// halved): the parallel infill lines reinforce while the surrounding perimeter cancels.
static std::map<double, double> interface_fill_angle_by_layer(const std::string &gcode)
{
    std::map<double, std::pair<double, double>> acc; // z -> summed length*(cos2a, sin2a)
    GCodeReader parser;
    parser.parse_buffer(gcode, [&acc](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (! line.extruding(self)) return;
        if (line.comment().find("support material interface") == std::string_view::npos) return;
        const double dx = line.dist_X(self), dy = line.dist_Y(self);
        const double len = std::hypot(dx, dy);
        if (len < 1e-6) return;
        const double a2 = 2.0 * std::atan2(dy, dx);
        auto &p = acc[self.z()];
        p.first  += len * std::cos(a2);
        p.second += len * std::sin(a2);
    });
    std::map<double, double> out;
    for (const auto &kv : acc) {
        double a = 0.5 * std::atan2(kv.second.second, kv.second.first);
        if (a < 0) a += M_PI;
        out[kv.first] = a;
    }
    return out;
}

// Acute angle (degrees) between two axial fill directions in [0, pi).
static double axial_angle_diff_deg(double a, double b)
{
    const double d = std::fmod(std::fabs(a - b), M_PI);
    return std::min(d, M_PI - d) * 180.0 / M_PI;
}

// Denser interface spacing yields more extruded length.
static double support_interface_extrusion_length(const std::string &gcode)
{
    double len = 0;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&len](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (! line.extruding(self)) return;
        if (line.comment().find("support material interface") == std::string_view::npos) return;
        len += std::hypot(line.dist_X(self), line.dist_Y(self));
    });
    return len;
}

// A cap slab overhanging a base, joined by a central stem: the cap can only be supported by resting on the
// base, forcing a genuine bottom contact. A horizontal tunnel does not work here -- tree/organic can arch a
// branch in from the opening and avoid the floor entirely.
static TriangleMesh support_capital()
{
    TriangleMesh model = make_cube(40, 40, 2);                              // base  [0,40]x[0,40]x[0,2]
    TriangleMesh stem  = make_cube(8, 8, 12);   stem.translate(16, 16, 1);  // stem  centered, z 1..13
    TriangleMesh cap   = make_cube(40, 40, 2);  cap.translate(0, 0, 12);    // cap   z 12..14
    model.merge(stem);
    model.merge(cap);
    return model;
}

TEST_CASE("Three raft layers are created", "[SupportMaterial]")
{
	Slic3r::Print print;
	Slic3r::Test::init_and_process_print({ cube(20) }, print, {
        { "enable_support", 1 },
        { "raft_layers",    3 }
		});
    REQUIRE(print.objects().front()->support_layers().size() == 3);
}

TEST_CASE("Enforced support layers are generated", "[SupportMaterial]")
{
    // enforce_support_layers forces support on the first N layers even with support off.
    Slic3r::Print baseline;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, baseline, {
        { "enable_support",         0 },
        { "enforce_support_layers", 0 }
    });
    REQUIRE(baseline.objects().front()->support_layers().empty());

    Slic3r::Print enforced;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, enforced, {
        { "enable_support",         0 },
        { "enforce_support_layers", 100 }
    });
    REQUIRE(enforced.objects().front()->support_layers().size() > 0);
}

TEST_CASE("SupportMaterial: Organic support keeps the synchronized grid when independent heights are disabled", "[SupportMaterial][OrganicTree]")
{
    DynamicPrintConfig config = organic_support_config(false, false, 0.2, 0.15);
    Print print;
    init_and_process_print({TestMesh::overhang}, print, config);

    const std::vector<const SupportLayer *> layers = nonempty_support_layers(print);
    REQUIRE_FALSE(layers.empty());
    for (const SupportLayer *layer : layers)
        REQUIRE(is_on_regular_grid(layer->print_z, 0.2));
}

TEST_CASE("SupportMaterial: Organic top contact can use an exact independent height", "[SupportMaterial][OrganicTree]")
{
    DynamicPrintConfig config = organic_support_config(false, true, 0.2, 0.15);
    Print print;
    init_and_process_print({TestMesh::overhang}, print, config);

    const std::vector<const SupportLayer *> layers = nonempty_support_layers(print);
    REQUIRE_FALSE(layers.empty());
    REQUIRE(std::any_of(layers.begin(), layers.end(), [](const SupportLayer *layer) {
        return std::abs(layer->height - 0.05) <= 1e-4;
    }));
    REQUIRE(std::any_of(layers.begin(), layers.end(), [](const SupportLayer *layer) {
        return !is_on_regular_grid(layer->print_z, 0.2);
    }));
}

TEST_CASE("SupportMaterial: Organic support body can use an independent output grid", "[SupportMaterial][OrganicTree]")
{
    DynamicPrintConfig legacy_config = organic_support_config(false, false, 0.1, 0.15);
    Print legacy_print;
    init_and_process_print({TestMesh::overhang}, legacy_print, legacy_config);
    const std::vector<const SupportLayer *> legacy_layers = nonempty_support_layers(legacy_print);

    DynamicPrintConfig independent_config = organic_support_config(true, true, 0.1, 0.15);
    Print independent_print;
    init_and_process_print({TestMesh::overhang}, independent_print, independent_config);
    const std::vector<const SupportLayer *> independent_layers = nonempty_support_layers(independent_print);

    DynamicPrintConfig full_only_config = organic_support_config(true, false, 0.1, 0.15);
    Print full_only_print;
    init_and_process_print({TestMesh::overhang}, full_only_print, full_only_config);
    const std::vector<const SupportLayer *> full_only_layers = nonempty_support_layers(full_only_print);

    REQUIRE_FALSE(legacy_layers.empty());
    REQUIRE_FALSE(independent_layers.empty());
    REQUIRE(full_only_layers.size() == independent_layers.size());
    REQUIRE(independent_layers.size() < legacy_layers.size());
    REQUIRE(std::any_of(independent_layers.begin(), independent_layers.end(), [](const SupportLayer *layer) {
        return layer->height > 0.1 + 1e-4;
    }));

    double previous_z = 0.;
    for (size_t layer_idx = 0; layer_idx < independent_layers.size(); ++layer_idx) {
        const SupportLayer *layer = independent_layers[layer_idx];
        REQUIRE(layer->print_z > previous_z + EPSILON);
        REQUIRE(layer->height >= 0.05 - EPSILON);
        REQUIRE(layer->height <= 0.4 + EPSILON);
        REQUIRE(full_only_layers[layer_idx]->print_z == Catch::Approx(layer->print_z).margin(EPSILON));
        previous_z = layer->print_z;
    }
}

TEST_CASE("SupportMaterial: Organic sub-minimum contact remainder is merged into a valid output layer", "[SupportMaterial][OrganicTree]")
{
    DynamicPrintConfig baseline_config = organic_support_config(
        false, false, 0.2, 0.17, 0.05, 0.266667);
    Print baseline_print;
    init_and_process_print({TestMesh::overhang}, baseline_print, baseline_config);
    const std::vector<const SupportLayer *> baseline_layers = nonempty_support_layers(baseline_print);
    REQUIRE_FALSE(baseline_layers.empty());
    for (const SupportLayer *layer : baseline_layers)
        REQUIRE(is_on_regular_grid(layer->print_z, 0.2));

    for (bool independent_body : {false, true}) {
        CAPTURE(independent_body);
        DynamicPrintConfig config = organic_support_config(
            independent_body, true, 0.2, 0.17, 0.05, 0.266667);
        Print print;
        init_and_process_print({TestMesh::overhang}, print, config);

        const std::vector<const SupportLayer *> layers = nonempty_support_layers(print);
        REQUIRE_FALSE(layers.empty());
        REQUIRE(std::any_of(layers.begin(), layers.end(), [](const SupportLayer *layer) {
            return !is_on_regular_grid(layer->print_z, 0.2);
        }));
        REQUIRE(std::any_of(layers.begin(), layers.end(), [](const SupportLayer *layer) {
            return is_on_regular_grid(layer->print_z - 0.03, 0.2);
        }));
        const bool same_as_baseline = layers.size() == baseline_layers.size() &&
            std::equal(layers.begin(), layers.end(), baseline_layers.begin(),
                [](const SupportLayer *lhs, const SupportLayer *rhs) {
                    return std::abs(lhs->print_z - rhs->print_z) <= EPSILON;
                });
        REQUIRE_FALSE(same_as_baseline);
        for (const SupportLayer *layer : layers) {
            REQUIRE(layer->height >= 0.05 - EPSILON);
            REQUIRE(layer->height <= 0.266667 + EPSILON);
        }
    }
}

TEST_CASE("SupportMaterial: Organic impossible exact contact plan falls back atomically", "[SupportMaterial][OrganicTree]")
{
    DynamicPrintConfig config = organic_support_config(false, true, 0.2, 0.17, 0.15, 0.2);
    Print print;
    init_and_process_print({TestMesh::overhang}, print, config);

    const std::vector<const SupportLayer *> layers = nonempty_support_layers(print);
    REQUIRE_FALSE(layers.empty());
    for (const SupportLayer *layer : layers) {
        REQUIRE(layer->height >= 0.15 - EPSILON);
        REQUIRE(is_on_regular_grid(layer->print_z, 0.2));
    }
}

TEST_CASE("SupportMaterial: Organic independent output handles raft and model contacts", "[SupportMaterial][OrganicTree]")
{
    SECTION("raft with zero top gap") {
        DynamicPrintConfig config = organic_support_config(true, true, 0.2, 0.0);
        config.set_deserialize_strict({{"raft_layers", 3}});
        Print print;
        init_and_process_print({TestMesh::overhang}, print, config);

        const std::vector<const SupportLayer *> layers = nonempty_support_layers(print);
        REQUIRE(layers.size() >= 3);
        double previous_z = 0.;
        for (const SupportLayer *layer : layers) {
            REQUIRE(layer->print_z > previous_z + EPSILON);
            REQUIRE(layer->height > EPSILON);
            previous_z = layer->print_z;
        }
    }

    SECTION("support resting on the model") {
        TriangleMesh mesh = Slic3r::Test::mesh(TestMesh::cube_with_hole);
        mesh.rotate_x(float(M_PI / 2));
        DynamicPrintConfig config = organic_support_config(true, true, 0.1, 0.15);
        config.set_deserialize_strict({
            {"support_on_build_plate_only", false},
            {"support_interface_bottom_layers", 2}
        });
        Print print;
        init_and_process_print({mesh}, print, config);

        const std::vector<const SupportLayer *> layers = nonempty_support_layers(print);
        REQUIRE_FALSE(layers.empty());
        double previous_z = 0.;
        for (const SupportLayer *layer : layers) {
            REQUIRE(layer->print_z > previous_z + EPSILON);
            REQUIRE(layer->height > EPSILON);
            previous_z = layer->print_z;
        }
    }
}

TEST_CASE("SupportMaterial: manual tree roof fragments require a printable contact", "[SupportMaterial][TreeSupport]")
{
    const Flow flow(0.4, 0.2, 0.4);
    const ExPolygon tiny_fragment = rectangular_area(0., 0., 0.5, 0.5);
    const ExPolygon main_contact = rectangular_area(0., 0., 10., 2.);

    SECTION("An expansion-only loop shorter than one nozzle circumference is discarded") {
        const ExPolygons original_contact = { rectangular_area(2., 2., 3., 3.) };
        REQUIRE(TreeSupportInternal::should_discard_manual_roof_fragment(
            true, tiny_fragment, original_contact, flow));
    }

    SECTION("A main interface with valid infill is preserved") {
        const ExPolygons original_contact = { main_contact };
        REQUIRE_FALSE(TreeSupportInternal::should_discard_manual_roof_fragment(
            true, main_contact, original_contact, flow));
    }

    SECTION("A genuine small manual contact that can carry a line is preserved") {
        const ExPolygons original_contact = { tiny_fragment };
        REQUIRE_FALSE(TreeSupportInternal::should_discard_manual_roof_fragment(
            true, tiny_fragment, original_contact, flow));
    }

    SECTION("Automatic tree interfaces are outside the manual fragment filter") {
        REQUIRE_FALSE(TreeSupportInternal::should_discard_manual_roof_fragment(
            false, tiny_fragment, {}, flow));
    }
}

TEST_CASE("SupportMaterial: Shared raft footprint modes preserve required support roots", "[SupportMaterial][Raft]")
{
    ExPolygon object_area = rectangular_area(0., 0., 20., 20.);
    Polygon hole = rectangular_area(5., 5., 15., 15.).contour;
    hole.make_clockwise();
    object_area.holes.emplace_back(std::move(hole));
    ExPolygons object_areas{object_area};
    ExPolygons support_areas{rectangular_area(22., 8., 24., 12.)};

    ExPolygons original = build_raft_first_layer_footprint(object_areas, support_areas, false, false);
    REQUIRE_FALSE(areas_contain(original, 10., 10.));
    REQUIRE(areas_contain(original, 23., 10.));

    ExPolygons solid = build_raft_first_layer_footprint(object_areas, support_areas, false, true);
    REQUIRE(areas_contain(solid, 10., 10.));
    REQUIRE(areas_contain(solid, 23., 10.));
    REQUIRE_FALSE(areas_contain(solid, 23., 18.));

    ExPolygons box = build_raft_first_layer_footprint(object_areas, support_areas, true, true);
    REQUIRE(areas_contain(box, 10., 10.));
    REQUIRE(areas_contain(box, 23., 10.));
    REQUIRE(areas_contain(box, 23., 18.));
}

TEST_CASE("SupportMaterial: Overlapping raft instances clip later paths in world coordinates", "[SupportMaterial][Raft]")
{
    ExtrusionEntityCollection paths = horizontal_raft_path(0., 10., 5.);
    ExtrusionPath interface_path(erSupportMaterialInterface, 0.12, 0.6f, 0.3f);
    interface_path.polyline = Polyline3(Polyline{
        Point::new_scale(0., 5.),
        Point::new_scale(10., 5.)});
    ExtrusionEntityCollection interface_paths;
    interface_paths.no_sort = true;
    interface_paths.append(std::move(interface_path));
    ExPolygons footprint{rectangular_area(0., 0., 10., 10.)};
    const Point overlap_shift = Point::new_scale(5., 0.);
    const Point separate_shift = Point::new_scale(20., 0.);

    const std::vector<RaftPathInstance> instances{
        {&paths, &footprint, Point{}},
        {&interface_paths, &footprint, overlap_shift},
        {&paths, &footprint, separate_shift},
        {&paths, &footprint, Point{}}};
    auto clipped = clip_overlapping_raft_paths(instances);

    REQUIRE(clipped.size() == instances.size());
    REQUIRE(clipped[0] == nullptr);
    REQUIRE(clipped[1] != nullptr);
    REQUIRE_FALSE(clipped[1]->empty());
    REQUIRE(clipped[1]->total_volume() < interface_paths.total_volume() - EPSILON);
    REQUIRE(clipped[1]->no_sort);
    const auto *remaining_path = dynamic_cast<const ExtrusionPath *>(clipped[1]->entities.front());
    REQUIRE(remaining_path != nullptr);
    REQUIRE(remaining_path->role() == erSupportMaterialInterface);
    REQUIRE(remaining_path->mm3_per_mm == Catch::Approx(0.12));
    REQUIRE(remaining_path->width == Catch::Approx(0.6));
    REQUIRE(remaining_path->height == Catch::Approx(0.3));
    REQUIRE(clipped[2] == nullptr);
    REQUIRE(clipped[3] != nullptr);
    REQUIRE(clipped[3]->empty());
}

TEST_CASE("SupportMaterial: Compatible overlapping rafts form one boolean-union toolpath", "[SupportMaterial][Raft]")
{
    const bool shared_object = GENERATE(false, true);
    CAPTURE(shared_object);

    Print print;
    Model model;
    process_overlapping_box_rafts(print, model, shared_object);

    std::vector<RaftPathInstance> instances;
    for (const PrintObject *object : print.objects()) {
        REQUIRE_FALSE(object->support_layers().empty());
        const SupportLayer *layer = object->support_layers().front();
        REQUIRE_FALSE(layer->support_islands.empty());
        for (const PrintInstance &instance : object->instances())
            instances.push_back({
                &layer->support_fills, &layer->support_islands, instance.shift,
                object, layer});
    }
    REQUIRE(instances.size() == 3);

    const double separate_volume = instances.front().paths->total_volume();
    auto merged = merge_overlapping_raft_paths(instances);
    REQUIRE(merged.size() == instances.size());
    REQUIRE(merged.front() != nullptr);
    REQUIRE_FALSE(merged.front()->empty());
    REQUIRE(merged.front()->total_volume() > separate_volume + EPSILON);
    REQUIRE(merged.front()->total_volume() < separate_volume * instances.size() - EPSILON);
    for (size_t index = 1; index < merged.size(); ++index) {
        REQUIRE(merged[index] != nullptr);
        REQUIRE(merged[index]->empty());
    }

    instances[1].support_extruder = 1;
    auto incompatible = merge_overlapping_raft_paths(instances);
    REQUIRE(incompatible.front() == nullptr);
    REQUIRE(incompatible[1] != nullptr);
    REQUIRE_FALSE(incompatible[1]->empty());
}

TEST_CASE("SupportMaterial: Every support style boolean-unions every configured raft layer", "[SupportMaterial][Raft]")
{
    struct StyleCase {
        const char *style;
        bool        tree;
    };
    const std::array styles{
        StyleCase{"grid", false}, StyleCase{"snug", false},
        StyleCase{"tree_slim", true}, StyleCase{"tree_strong", true},
        StyleCase{"tree_hybrid", true}, StyleCase{"organic", true}};

    for (const StyleCase &style : styles) {
        CAPTURE(style.style);
        Print print;
        Model model;
        process_overlapping_box_rafts(print, model, false, style.style, style.tree, 4);
        REQUIRE(print.objects().size() == 3);

        for (size_t raft_layer = 0; raft_layer < 4; ++raft_layer) {
            CAPTURE(raft_layer);
            std::vector<RaftPathInstance> instances;
            for (const PrintObject *object : print.objects()) {
                REQUIRE(object->support_layers().size() > raft_layer);
                const SupportLayer *layer = object->support_layers()[raft_layer];
                REQUIRE_FALSE(layer->support_islands.empty());
                instances.push_back({
                    &layer->support_fills, &layer->support_islands,
                    object->instances().front().shift, object, layer});
            }

            auto merged = merge_overlapping_raft_paths(instances);
            REQUIRE(merged.front() != nullptr);
            REQUIRE_FALSE(merged.front()->empty());
            for (size_t index = 1; index < merged.size(); ++index) {
                REQUIRE(merged[index] != nullptr);
                REQUIRE(merged[index]->empty());
            }
        }
    }
}

TEST_CASE("SupportMaterial: Raft overlap suppression keeps real conflict checks", "[SupportMaterial][Raft]")
{
    const Line horizontal(Point::new_scale(0., 0.), Point::new_scale(10., 0.));
    const Line vertical(Point::new_scale(5., -5.), Point::new_scale(5., 5.));
    const int first_id = 1;
    const int second_id = 2;

    REQUIRE(ConflictChecker::line_intersect(
        LineWithID(horizontal, &first_id, erPerimeter),
        LineWithID(vertical, &second_id, erPerimeter)).has_value());
    REQUIRE_FALSE(ConflictChecker::line_intersect(
        LineWithID(horizontal, &first_id, erSupportMaterial, true),
        LineWithID(vertical, &second_id, erPerimeter)).has_value());
    REQUIRE(ConflictChecker::line_intersect(
        LineWithID(horizontal, &first_id, erSupportMaterial, true),
        LineWithID(vertical, &second_id, erWipeTower)).has_value());
}

TEST_CASE("SupportMaterial: Overlapping object rafts emit less first-layer extrusion than separated rafts", "[SupportMaterial][Raft][GCode]")
{
    const bool shared_object = GENERATE(false, true);
    CAPTURE(shared_object);
    const double overlapping = two_object_box_raft_first_layer_extrusion(25., shared_object);
    const double separated = two_object_box_raft_first_layer_extrusion(70., shared_object);

    REQUIRE(overlapping > EPSILON);
    REQUIRE(separated > EPSILON);
    REQUIRE(overlapping < separated * 0.9);
}

TEST_CASE("SupportMaterial: Raft layer expansion step grows cumulatively downward", "[SupportMaterial][Raft]")
{
    struct StyleCase {
        const char *style;
        bool        tree;
    };
    const std::array styles{
        StyleCase{"grid", false}, StyleCase{"snug", false},
        StyleCase{"tree_slim", true}, StyleCase{"tree_strong", true},
        StyleCase{"tree_hybrid", true}, StyleCase{"organic", true}};

    constexpr int raft_layers = 4;
    constexpr double step = 1.0;
    for (const StyleCase &style : styles) {
        CAPTURE(style.style);
        DynamicPrintConfig baseline = raft_support_config(style.style, style.tree, raft_layers);
        baseline.set_deserialize_strict({
            {"enable_support", false},
            {"raft_generate_bounding_box", true},
            {"raft_expansion", 0.0},
            {"raft_first_layer_expansion", 0.0},
            {"raft_layer_expansion_step", 0.0}
        });
        DynamicPrintConfig stepped = baseline;
        stepped.set_deserialize_strict("raft_layer_expansion_step", std::to_string(step));

        const std::vector<double> baseline_widths = raft_layer_widths(TestMesh::cube_20x20x20, baseline);
        const std::vector<double> stepped_widths = raft_layer_widths(TestMesh::cube_20x20x20, stepped);
        REQUIRE(baseline_widths.size() == raft_layers);
        REQUIRE(stepped_widths.size() == raft_layers);
        for (size_t layer_id = 0; layer_id < stepped_widths.size(); ++layer_id) {
            const double expected_width_increase =
                2.0 * step * double(raft_layers - layer_id - 1);
            CAPTURE(layer_id, baseline_widths[layer_id], stepped_widths[layer_id], expected_width_increase);
            REQUIRE(stepped_widths[layer_id] - baseline_widths[layer_id] ==
                    Catch::Approx(expected_width_increase).margin(0.02));
        }

        DynamicPrintConfig single_baseline = raft_support_config(style.style, style.tree, 1);
        single_baseline.set_deserialize_strict({
            {"enable_support", false},
            {"raft_generate_bounding_box", true},
            {"raft_expansion", 0.0},
            {"raft_first_layer_expansion", 0.0},
            {"raft_layer_expansion_step", 0.0}
        });
        DynamicPrintConfig single_stepped = single_baseline;
        single_stepped.set_deserialize_strict("raft_layer_expansion_step", "5");
        const auto single_baseline_widths = raft_layer_widths(TestMesh::cube_20x20x20, single_baseline);
        const auto single_stepped_widths = raft_layer_widths(TestMesh::cube_20x20x20, single_stepped);
        REQUIRE(single_baseline_widths.size() == 1);
        REQUIRE(single_stepped_widths.size() == 1);
        REQUIRE(single_stepped_widths.front() == Catch::Approx(single_baseline_widths.front()).margin(EPSILON));
    }
}

TEST_CASE("SupportMaterial: Raft layer expansion step remains additive with existing expansion options", "[SupportMaterial][Raft]")
{
    DynamicPrintConfig stepped = raft_support_config("grid", false, 4);
    stepped.set_deserialize_strict({
        {"enable_support", false},
        {"raft_generate_bounding_box", true},
        {"raft_expansion", 0.0},
        {"raft_first_layer_expansion", 0.0},
        {"raft_layer_expansion_step", 1.0}
    });
    DynamicPrintConfig uniformly_expanded = stepped;
    uniformly_expanded.set_deserialize_strict("raft_expansion", "6");
    DynamicPrintConfig first_layer_expanded = stepped;
    first_layer_expanded.set_deserialize_strict("raft_first_layer_expansion", "2");

    const auto stepped_widths = raft_layer_widths(TestMesh::cube_20x20x20, stepped);
    const auto uniform_widths = raft_layer_widths(TestMesh::cube_20x20x20, uniformly_expanded);
    const auto first_layer_widths = raft_layer_widths(TestMesh::cube_20x20x20, first_layer_expanded);
    REQUIRE(stepped_widths.size() == 4);
    REQUIRE(uniform_widths.size() == stepped_widths.size());
    REQUIRE(first_layer_widths.size() == stepped_widths.size());
    REQUIRE(raft_volume(TestMesh::cube_20x20x20, uniformly_expanded) >
            raft_volume(TestMesh::cube_20x20x20, stepped) + EPSILON);
    REQUIRE(raft_volume(TestMesh::cube_20x20x20, first_layer_expanded) >
            raft_volume(TestMesh::cube_20x20x20, stepped) + EPSILON);
    REQUIRE(first_layer_widths.front() > stepped_widths.front() + EPSILON);
    for (size_t layer_id = 1; layer_id < stepped_widths.size(); ++layer_id)
        REQUIRE(first_layer_widths[layer_id] == Catch::Approx(stepped_widths[layer_id]).margin(EPSILON));
}

TEST_CASE("SupportMaterial: Rafts that overlap only after downward expansion are boolean-unioned", "[SupportMaterial][Raft][GCode]")
{
    const double overlapping = two_object_box_raft_first_layer_extrusion(21., false, 0., 2., 2);
    const double separated = two_object_box_raft_first_layer_extrusion(50., false, 0., 2., 2);

    REQUIRE(overlapping > EPSILON);
    REQUIRE(separated > EPSILON);
    REQUIRE(overlapping < separated - EPSILON);
}

TEST_CASE("SupportMaterial: All support styles accept every raft footprint mode", "[SupportMaterial][Raft]")
{
    struct StyleCase {
        const char *style;
        bool        tree;
    };
    const std::array styles{
        StyleCase{"grid", false}, StyleCase{"snug", false},
        StyleCase{"tree_slim", true}, StyleCase{"tree_strong", true},
        StyleCase{"tree_hybrid", true}, StyleCase{"organic", true}};
    const std::array modes{
        std::pair{false, false}, std::pair{false, true}, std::pair{true, false}};

    for (const StyleCase &style : styles) {
        for (const auto &[box, ignore_internal] : modes) {
            CAPTURE(style.style, box, ignore_internal);
            DynamicPrintConfig config = raft_support_config(style.style, style.tree, 2);
            config.set_deserialize_strict({
                {"raft_generate_bounding_box", box},
                {"raft_ignore_internal_contours", ignore_internal}
            });
            Print print;
            init_and_process_print({TestMesh::cube_with_hole}, print, config);

            REQUIRE(print.objects().front()->slicing_parameters().raft_layers() == 2);
            REQUIRE(extruded_raft_layer_count(print) == 2);
        }
    }
}

TEST_CASE("SupportMaterial: Every support style changes paths for solid and box raft modes", "[SupportMaterial][Raft]")
{
    struct StyleCase {
        const char *style;
        bool        tree;
    };
    const std::array styles{
        StyleCase{"grid", false}, StyleCase{"snug", false},
        StyleCase{"tree_slim", true}, StyleCase{"tree_strong", true},
        StyleCase{"tree_hybrid", true}, StyleCase{"organic", true}};

    for (const StyleCase &style : styles) {
        CAPTURE(style.style);
        DynamicPrintConfig original = raft_support_config(style.style, style.tree, 2);
        original.set_deserialize_strict({
            {"enable_support", false},
            {"raft_generate_bounding_box", false},
            {"raft_ignore_internal_contours", false}
        });
        DynamicPrintConfig solid = original;
        solid.set_deserialize_strict("raft_ignore_internal_contours", "1");
        const double original_ring_volume = raft_volume(TestMesh::cube_with_hole, original);
        const double solid_ring_volume = raft_volume(TestMesh::cube_with_hole, solid);
        REQUIRE(solid_ring_volume > original_ring_volume + EPSILON);

        DynamicPrintConfig box = original;
        box.set_deserialize_strict("raft_generate_bounding_box", "1");
        const double original_separate_volume = raft_volume(TestMesh::two_hollow_squares, original);
        const double box_separate_volume = raft_volume(TestMesh::two_hollow_squares, box);
        REQUIRE(box_separate_volume > original_separate_volume + EPSILON);
    }
}

TEST_CASE("SupportMaterial: Non-organic tree raft layer counts are exact", "[SupportMaterial][Raft]")
{
    for (const char *style : {"tree_slim", "tree_strong", "tree_hybrid"}) {
        for (int raft_layers = 1; raft_layers <= 4; ++raft_layers) {
            CAPTURE(style, raft_layers);
            DynamicPrintConfig config = raft_support_config(style, true, raft_layers);
            Print print;
            init_and_process_print({TestMesh::cube_20x20x20}, print, config);

            const SlicingParameters &params = print.objects().front()->slicing_parameters();
            REQUIRE(params.raft_layers() == size_t(raft_layers));
            REQUIRE(extruded_raft_layer_count(print) == size_t(raft_layers));
        }
    }
}

TEST_CASE("SupportMaterial: Raft layer heights honor support nozzle limits", "[SupportMaterial][Raft]")
{
    struct HeightCase {
        double nozzle;
        double maximum;
    };
    const std::array cases{
        HeightCase{0.4, 0.3}, HeightCase{0.6, 0.45}, HeightCase{0.8, 0.6},
        HeightCase{1.2, 0.9}, HeightCase{0.8, 0.35}};

    for (const HeightCase &height_case : cases) {
        CAPTURE(height_case.nozzle, height_case.maximum);
        DynamicPrintConfig config = raft_support_config("grid", false, 3);
        config.set_deserialize_strict("nozzle_diameter", std::to_string(height_case.nozzle));
        config.set_deserialize_strict("max_layer_height", std::to_string(height_case.maximum));
        Print print;
        init_and_process_print({TestMesh::cube_20x20x20}, print, config);

        const SlicingParameters &params = print.objects().front()->slicing_parameters();
        const double expected = std::min(0.75 * height_case.nozzle, height_case.maximum);
        REQUIRE(params.base_raft_layer_height == Catch::Approx(expected).margin(EPSILON));
        REQUIRE(params.interface_raft_layer_height == Catch::Approx(expected).margin(EPSILON));
        REQUIRE(params.contact_raft_layer_height == Catch::Approx(expected).margin(EPSILON));
        REQUIRE(params.base_raft_layer_height <= height_case.maximum + EPSILON);
        REQUIRE(params.interface_raft_layer_height <= height_case.maximum + EPSILON);
        REQUIRE(params.contact_raft_layer_height <= height_case.maximum + EPSILON);
    }
}

TEST_CASE("SupportMaterial: Raft Z distance is exact or object-grid rounded", "[SupportMaterial][Raft]")
{
    for (const auto &[independent, distance, expected] : {
             std::tuple{false, 0.17, 0.2},
             std::tuple{true, 0.17, 0.17},
             std::tuple{false, 0.0, 0.0},
             std::tuple{true, 0.0, 0.0}}) {
        CAPTURE(independent, distance, expected);
        DynamicPrintConfig config = raft_support_config("grid", false, 2);
        config.set_deserialize_strict({
            {"raft_contact_distance", distance},
            {"independent_support_layer_height", independent}
        });
        Print print;
        init_and_process_print({TestMesh::cube_20x20x20}, print, config);
        REQUIRE(print.objects().front()->slicing_parameters().gap_raft_object ==
                Catch::Approx(expected).margin(EPSILON));
    }
}

TEST_CASE("SupportMaterial: Raft expansion density spacing and pattern affect paths", "[SupportMaterial][Raft]")
{
    DynamicPrintConfig baseline = raft_support_config("grid", false, 4);
    baseline.set_deserialize_strict({
        {"enable_support", false},
        {"raft_generate_bounding_box", true},
        {"raft_ignore_internal_contours", false},
        {"raft_expansion", 0.0},
        {"raft_first_layer_expansion", 0.0}
    });

    DynamicPrintConfig expanded = baseline;
    expanded.set_deserialize_strict("raft_expansion", "3");
    REQUIRE(raft_volume(TestMesh::cube_20x20x20, expanded) >
            raft_volume(TestMesh::cube_20x20x20, baseline) + EPSILON);

    DynamicPrintConfig first_layer_expanded = baseline;
    first_layer_expanded.set_deserialize_strict("raft_first_layer_expansion", "3");
    REQUIRE(raft_volume(TestMesh::cube_20x20x20, first_layer_expanded) >
            raft_volume(TestMesh::cube_20x20x20, baseline) + EPSILON);

    DynamicPrintConfig low_density = baseline;
    low_density.set_deserialize_strict("raft_first_layer_density", "30");
    DynamicPrintConfig high_density = baseline;
    high_density.set_deserialize_strict("raft_first_layer_density", "100");
    REQUIRE(raft_volume(TestMesh::cube_20x20x20, high_density) >
            raft_volume(TestMesh::cube_20x20x20, low_density) + EPSILON);

    DynamicPrintConfig wide_spacing = baseline;
    wide_spacing.set_deserialize_strict("raft_base_pattern_spacing", "5");
    DynamicPrintConfig narrow_spacing = baseline;
    narrow_spacing.set_deserialize_strict("raft_base_pattern_spacing", "0.05");
    REQUIRE(raft_volume(TestMesh::cube_20x20x20, narrow_spacing) >
            raft_volume(TestMesh::cube_20x20x20, wide_spacing) + EPSILON);

    DynamicPrintConfig rectilinear = baseline;
    rectilinear.set_deserialize_strict("raft_base_pattern", "rectilinear");
    DynamicPrintConfig honeycomb = baseline;
    honeycomb.set_deserialize_strict("raft_base_pattern", "honeycomb");
    DynamicPrintConfig concentric = baseline;
    concentric.set_deserialize_strict("raft_base_pattern", "concentric");
    REQUIRE(std::abs(raft_volume(TestMesh::cube_20x20x20, rectilinear) -
                     raft_volume(TestMesh::cube_20x20x20, honeycomb)) > EPSILON);
    REQUIRE(raft_volume(TestMesh::cube_20x20x20, concentric) > EPSILON);
    REQUIRE(std::abs(raft_volume(TestMesh::cube_20x20x20, rectilinear) -
                     raft_volume(TestMesh::cube_20x20x20, concentric)) > EPSILON);
}

TEST_CASE("SupportMaterial: Concentric raft fills a nozzle-width center remainder", "[SupportMaterial][Raft]")
{
    const Flow  flow(0.6f, 0.2f, 0.6f);
    const float gap = 0.05f;
    const float density = flow.spacing() / (flow.spacing() + gap);
    const double inset_distance = flow.spacing() / density;
    const double side = 8. * inset_distance + flow.width() + 0.2;
    const Point center = Point::new_scale(0.5 * side, 0.5 * side);

    auto center_is_covered = [&](bool fill_concentric_gaps) {
        std::unique_ptr<Fill> filler(Fill::new_from_type(ipConcentric));
        filler->spacing = flow.spacing();
        filler->set_bounding_box(BoundingBox(Point::new_scale(0., 0.), Point::new_scale(side, side)));

        FillParams params;
        params.density = density;
        params.dont_adjust = true;
        params.flow = flow;
        params.fill_concentric_gaps = fill_concentric_gaps;

        Surface surface(stInternal, rectangular_area(0., 0., side, side));
        const Polylines paths = filler->fill_surface(&surface, params);
        const ExPolygons coverage = union_ex(offset(paths, 0.5f * float(flow.scaled_width())));
        const bool covered = std::any_of(coverage.begin(), coverage.end(), [&center](const ExPolygon &area) {
            return area.contains(center);
        });
        return std::pair{covered, paths.size()};
    };

    const auto legacy = center_is_covered(false);
    const auto raft = center_is_covered(true);
    CAPTURE(legacy.second, raft.second);
    REQUIRE_FALSE(legacy.first);
    REQUIRE(raft.first);
    REQUIRE(raft.second > legacy.second);
}

TEST_CASE("SupportMaterial: Concentric raft fills a partially collapsed branch", "[SupportMaterial][Raft]")
{
    const Flow  flow(0.6f, 0.2f, 0.6f);
    const float gap = 0.05f;
    const float density = flow.spacing() / (flow.spacing() + gap);
    const std::array<Point, 3> branch_centers = {
        Point::new_scale(-6., 0.), Point::new_scale(-4., 0.), Point::new_scale(-2., 0.)};

    ExPolygon area;
    area.contour.points = {
        Point::new_scale(-8., -1.3), Point::new_scale(0., -1.3),
        Point::new_scale(0., -4.), Point::new_scale(8., -4.),
        Point::new_scale(8., 4.), Point::new_scale(0., 4.),
        Point::new_scale(0., 1.3), Point::new_scale(-8., 1.3)};
    area.contour.make_counter_clockwise();

    auto branch_is_covered = [&](bool fill_concentric_gaps) {
        std::unique_ptr<Fill> filler(Fill::new_from_type(ipConcentric));
        filler->spacing = flow.spacing();
        filler->set_bounding_box(area.contour.bounding_box());

        FillParams params;
        params.density = density;
        params.dont_adjust = true;
        params.flow = flow;
        params.fill_concentric_gaps = fill_concentric_gaps;

        Surface surface(stInternal, area);
        const Polylines paths = filler->fill_surface(&surface, params);
        const ExPolygons coverage = union_ex(offset(paths, 0.5f * float(flow.scaled_width())));
        const size_t covered_centers = std::count_if(branch_centers.begin(), branch_centers.end(), [&coverage](const Point &center) {
            return std::any_of(coverage.begin(), coverage.end(), [&center](const ExPolygon &covered_area) {
                return covered_area.contains(center);
            });
        });
        return std::pair{covered_centers, paths.size()};
    };

    const auto legacy = branch_is_covered(false);
    const auto raft = branch_is_covered(true);
    CAPTURE(legacy.second, raft.second);
    REQUIRE(legacy.first < branch_centers.size());
    REQUIRE(raft.first == branch_centers.size());
    REQUIRE(raft.second > legacy.second);
}

SCENARIO("Support layer Z honors contact distance", "[SupportMaterial]")
{
    // Box h = 20mm, hole bottom at 5mm, hole height 10mm (top edge at 15mm).
    TriangleMesh mesh = Slic3r::Test::mesh(Slic3r::Test::TestMesh::cube_with_hole);
    mesh.rotate_x(float(M_PI / 2));

	auto check = [](Slic3r::Print &print, bool &first_support_layer_height_ok, bool &layer_height_minimum_ok, bool &layer_height_maximum_ok)
	{
        ConstSupportLayerPtrsAdaptor support_layers = print.objects().front()->support_layers();

		first_support_layer_height_ok = support_layers.front()->print_z == print.config().initial_layer_print_height.value;

		layer_height_minimum_ok = true;
		layer_height_maximum_ok = true;
		double min_layer_height = print.config().min_layer_height.values.front();
		double max_layer_height = print.config().nozzle_diameter.values.front();
		if (print.config().max_layer_height.values.front() > EPSILON)
			max_layer_height = std::min(max_layer_height, print.config().max_layer_height.values.front());
		for (size_t i = 1; i < support_layers.size(); ++ i) {
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z < min_layer_height - EPSILON)
				layer_height_minimum_ok = false;
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z > max_layer_height + EPSILON)
				layer_height_maximum_ok = false;
		}
	};

    GIVEN("A print object having one modelObject") {
        WHEN("Layer height = 0.2 and first layer height = 0.4") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
                { "enable_support",             1 },
                { "layer_height",               0.2 },
                { "initial_layer_print_height", 0.4 },
                { "dont_support_bridges",       false },
			});
			bool first_layer_ok, layer_min_ok, layer_max_ok;
            check(print, first_layer_ok, layer_min_ok, layer_max_ok);
            THEN("First layer height is honored")			{ REQUIRE(first_layer_ok == true); }
            THEN("No null or negative support layers")		{ REQUIRE(layer_min_ok == true); }
            THEN("No layers thicker than nozzle diameter")	{ REQUIRE(layer_max_ok == true); }
        }
        WHEN("Layer height = 0.2 and first layer height = 0.3") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
                { "enable_support",             1 },
                { "layer_height",               0.2 },
                { "initial_layer_print_height", 0.3 },
                { "dont_support_bridges",       false },
            });
            bool first_layer_ok, layer_min_ok, layer_max_ok;
            check(print, first_layer_ok, layer_min_ok, layer_max_ok);
            THEN("First layer height is honored")			{ REQUIRE(first_layer_ok == true); }
            THEN("No null or negative support layers")		{ REQUIRE(layer_min_ok == true); }
            THEN("No layers thicker than nozzle diameter")	{ REQUIRE(layer_max_ok == true); }
        }
    }
}

// extrude_support once held a `static` lambda capturing `this`, so a second export in the
// same process dereferenced a returned stack frame (ASan: stack-use-after-return).
TEST_CASE("Support G-code emission survives a second slice in the same process", "[SupportMaterial][Regression]")
{
    const std::string first = slice({ TestMesh::overhang }, { { "enable_support", 1 } });
    REQUIRE(! layers_with_role(first, "support").empty());

    const std::string second = slice({ TestMesh::overhang }, { { "enable_support", 1 } });
    REQUIRE(! layers_with_role(second, "support").empty());
}

// The contact layer counts toward the configured interface layer count, so N configured top
// interface layers produce exactly N interface layers, not N+1.
TEST_CASE("Support top interface layer count matches the configured value", "[SupportMaterial]")
{
    const int top = GENERATE(1, 2, 3, 4, 6);
    const std::string g = slice({ TestMesh::overhang }, {
        { "enable_support",                  1 },
        { "layer_height",                    0.2 },
        { "support_on_build_plate_only",     1 },
        { "support_interface_top_layers",    top },
        { "support_interface_bottom_layers", 0 },
    });
    CAPTURE(top);
    REQUIRE(support_base_layer_count(g)      > 0);          // support actually formed
    REQUIRE(support_interface_layer_count(g) == size_t(top));
}

// A rotated cube-with-hole is a horizontal tunnel whose ceiling and floor both receive support, so top
// and bottom interfaces can be exercised independently (the floor is the bottom contact).
static TriangleMesh support_tunnel()
{
    TriangleMesh tunnel = Slic3r::Test::mesh(TestMesh::cube_with_hole);
    tunnel.rotate_x(float(M_PI / 2));
    return tunnel;
}

static size_t tunnel_interface_layers(const TriangleMesh &tunnel, int top, int bottom)
{
    const std::string g = slice({ tunnel }, {
        { "enable_support",                  1 },
        { "layer_height",                    0.2 },
        { "support_on_build_plate_only",     0 },
        { "support_interface_top_layers",    top },
        { "support_interface_bottom_layers", bottom },
    });
    REQUIRE(support_base_layer_count(g) > 0); // support actually formed
    return support_interface_layer_count(g);
}

TEST_CASE("No support interface is generated when neither top nor bottom is configured", "[SupportMaterial]")
{
    REQUIRE(tunnel_interface_layers(support_tunnel(), 0, 0) == 0);
}

TEST_CASE("Bottom interface layer count matches its setting with top interface off", "[SupportMaterial]")
{
    const int bottom = GENERATE(1, 3, 6);
    CAPTURE(bottom);
    REQUIRE(tunnel_interface_layers(support_tunnel(), 0, bottom) == size_t(bottom));
}

// support_interface_bottom_layers = -1 means "same as top".
TEST_CASE("Support interface bottom layers default to the top layer count", "[SupportMaterial]")
{
    const TriangleMesh tunnel = support_tunnel();
    REQUIRE(tunnel_interface_layers(tunnel, 0, -1) == tunnel_interface_layers(tunnel, 0, 0));
    REQUIRE(tunnel_interface_layers(tunnel, 3, -1) == tunnel_interface_layers(tunnel, 3, 3));
}

TEST_CASE("Default support still emits base and interface material", "[SupportMaterial][Regression]")
{
    const std::string g = slice({ TestMesh::overhang }, { { "enable_support", 1 } });
    REQUIRE(support_base_layer_count(g)      > 0);
    REQUIRE(support_interface_layer_count(g) > 0);
}

// Organic runs TreeSupport3D + TreeModelVolumes, the others the classic TreeSupport.cpp path.
TEST_CASE("Every tree support style produces base and interface material", "[SupportMaterial]")
{
    const char *style = GENERATE("organic", "tree_slim", "tree_strong", "tree_hybrid");
    INFO("style=" << style);
    const std::string g = slice({ TestMesh::overhang }, {
        { "enable_support",               1 },
        { "layer_height",                 0.2 },
        { "support_type",                 "tree(auto)" },
        { "support_style",                style },
        { "support_interface_top_layers", 3 },
    });
    CHECK(support_base_layer_count(g)      > 0);
    CHECK(support_interface_layer_count(g) > 0);
}

TEST_CASE("Spiral inset remains available to independent contact layers", "[SupportMaterial][Regression]")
{
    const auto inherited = support_contact_pattern_or_interface(smipAuto, smipSpiralInset);
    REQUIRE(inherited == smipSpiralInset);
    for (const double density : {0.2, 1.0}) {
        for (const bool zero_gap : {false, true}) {
            CHECK(support_interface_fill_pattern(inherited, density, zero_gap) == ipSpiralInset);
            CHECK(support_interface_fill_pattern(smipSpiralInset, density, zero_gap) == ipSpiralInset);
        }
    }
    CHECK(support_contact_pattern_or_interface(smipConcentric, smipSpiralInset) == smipConcentric);
}

TEST_CASE("Raft interface angle alternates by 45 degrees per interface id", "[SupportMaterial]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, print, { { "enable_support", 1 } });
    SupportParameters sp(*print.objects().front());
    sp.raft_angle_interface = 0.5f;
    REQUIRE_THAT(sp.raft_interface_angle(0), Catch::Matchers::WithinAbs(0.5 + M_PI / 4., 1e-6));
    REQUIRE_THAT(sp.raft_interface_angle(1), Catch::Matchers::WithinAbs(0.5 - M_PI / 4., 1e-6));
}

// The angle inputs are overwritten directly, so the pattern-to-angle mapping is checked
// independently of the sliced object's configuration.
TEST_CASE("Support interface fill angle follows the configured interface pattern", "[SupportMaterial]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, print, { { "enable_support", 1 } });
    SupportParameters sp(*print.objects().front());
    sp.interface_angle = 0.3f;
    sp.base_angle      = 1.1f;
    const double tol   = 1e-6;

    SECTION("Rectilinear shifts the interface angle by -45deg for snug support") {
        sp.support_interface_pattern = smipRectilinear;
        sp.support_style             = smsSnug;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle - M_PI_4, tol));
        REQUIRE_THAT(sp.support_interface_angle(3), Catch::Matchers::WithinAbs(sp.interface_angle - M_PI_4, tol));
    }
    SECTION("Rectilinear leaves the interface angle alone for the other styles") {
        sp.support_interface_pattern = smipRectilinear;
        sp.support_style             = smsGrid;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle, tol));
    }
    SECTION("Rectilinear interlaced alternates -/+45deg by interface id parity") {
        sp.support_interface_pattern = smipRectilinearInterlaced;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle - M_PI_4, tol));
        REQUIRE_THAT(sp.support_interface_angle(1), Catch::Matchers::WithinAbs(sp.interface_angle + M_PI_4, tol));
    }
    SECTION("Grid uses the base angle") {
        sp.support_interface_pattern = smipGrid;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.base_angle, tol));
    }
    SECTION("Auto and concentric use the interface angle unchanged") {
        sp.support_interface_pattern = smipAuto;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle, tol));
        sp.support_interface_pattern = smipConcentric;
        REQUIRE_THAT(sp.support_interface_angle(0), Catch::Matchers::WithinAbs(sp.interface_angle, tol));
    }
}

// End-to-end that the pattern reaches the emitted fill, not just support_interface_angle().
TEST_CASE("Interlaced support interface alternates fill angle while rectilinear does not", "[SupportMaterial]")
{
    auto interface_angles = [](const char *pattern) {
        std::vector<double> a;
        for (const auto &kv : interface_fill_angle_by_layer(slice({ TestMesh::overhang }, {
                 { "enable_support",               1 },
                 { "layer_height",                 0.2 },
                 { "support_on_build_plate_only",  1 },
                 { "support_interface_top_layers", 6 },
                 { "support_interface_pattern",    pattern } })))
            a.push_back(kv.second);
        return a;
    };

    const std::vector<double> rectilinear = interface_angles("rectilinear");
    const std::vector<double> interlaced  = interface_angles("rectilinear_interlaced");
    REQUIRE(rectilinear.size() >= 3);
    REQUIRE(interlaced.size()  >= 3);

    for (size_t i = 1; i < rectilinear.size(); ++i)
        REQUIRE(axial_angle_diff_deg(rectilinear[i], rectilinear[0]) < 15.0);

    for (size_t i = 1; i < interlaced.size(); ++i)
        REQUIRE(axial_angle_diff_deg(interlaced[i], interlaced[i - 1]) > 60.0);
}

// Normal and non-organic tree support share the same interface angle logic: with a rectilinear interface
// pattern both emit their interface fill at the same angle (both go through support_interface_angle()).
TEST_CASE("Normal and tree support use the same interface fill angle", "[SupportMaterial]")
{
    auto mean_interface_angle = [](const char *type, const char *style) {
        const auto angles = interface_fill_angle_by_layer(slice({ TestMesh::overhang }, {
            { "enable_support", 1 }, { "layer_height", 0.2 }, { "support_on_build_plate_only", 1 },
            { "support_type", type }, { "support_style", style },
            { "support_interface_top_layers", 6 }, { "support_interface_pattern", "rectilinear" } }));
        REQUIRE(angles.size() >= 3);
        // Axial mean, as in interface_fill_angle_by_layer: a plain mean would split angles either
        // side of the [0, pi) wrap.
        double x = 0, y = 0;
        for (const auto &kv : angles) {
            x += std::cos(2.0 * kv.second);
            y += std::sin(2.0 * kv.second);
        }
        double mean = 0.5 * std::atan2(y, x);
        if (mean < 0) mean += M_PI;
        return mean;
    };
    REQUIRE(axial_angle_diff_deg(mean_interface_angle("normal(auto)", "default"),
                                 mean_interface_angle("tree(auto)", "tree_slim")) < 10.0);
}

// Every style, because the non-organic tree styles once emitted one more top interface layer than the rest.
TEST_CASE("Top interface layer count equals the configured value for every support style", "[SupportMaterial]")
{
    auto [type, style] = GENERATE(table<const char *, const char *>({
        { "normal(auto)", "grid" },        { "normal(auto)", "snug" },
        { "tree(auto)",   "organic" },     { "tree(auto)",   "tree_slim" },
        { "tree(auto)",   "tree_strong" }, { "tree(auto)",   "tree_hybrid" },
    }));
    CAPTURE(style);
    const std::string g = slice({ TestMesh::overhang }, {
        { "enable_support",               1 },
        { "layer_height",                 0.2 },
        { "support_type",                 type },
        { "support_style",                style },
        { "support_interface_top_layers", 4 },
    });
    REQUIRE(support_interface_layer_count(g) == 4u);
}

// The bottom interface was dropped in earlier versions when support started on the model rather
// than the plate.
TEST_CASE("Non-organic tree support generates a bottom interface on internal geometry", "[SupportMaterial]")
{
    const std::string g = slice({ support_tunnel() }, {
        { "enable_support",                  1 },
        { "layer_height",                    0.2 },
        { "support_on_build_plate_only",     0 },
        { "support_type",                    "tree(auto)" },
        { "support_style",                   "tree_slim" },
        { "support_interface_top_layers",    0 },
        { "support_interface_bottom_layers", 6 },
    });
    REQUIRE(support_base_layer_count(g)      > 0);
    REQUIRE(support_interface_layer_count(g) > 0);
}

// The capital forces the model contact; on a horizontal tunnel organic can arch a branch in and make none.
TEST_CASE("A bottom interface is produced for every support style on a forced model contact", "[SupportMaterial]")
{
    auto [type, style] = GENERATE(table<const char *, const char *>({
        { "normal(auto)", "default" },     { "tree(auto)", "tree_slim" },
        { "tree(auto)",   "tree_strong" }, { "tree(auto)", "tree_hybrid" },
        { "tree(auto)",   "organic" },
    }));
    CAPTURE(style);
    REQUIRE(support_interface_layer_count(slice({ support_capital() }, {
        { "enable_support", 1 }, { "layer_height", 0.2 }, { "support_on_build_plate_only", 0 },
        { "support_type", type }, { "support_style", style },
        { "support_interface_top_layers", 0 }, { "support_interface_bottom_layers", 6 } })) > 0);
}

TEST_CASE("Bottom interface spacing controls bottom interface density for every support style", "[SupportMaterial]")
{
    auto [type, style] = GENERATE(table<const char *, const char *>({
        { "normal(auto)", "default" },     { "tree(auto)", "tree_slim" },
        { "tree(auto)",   "tree_strong" }, { "tree(auto)", "tree_hybrid" },
        { "tree(auto)",   "organic" },
    }));
    CAPTURE(style);
    const TriangleMesh model = support_capital();
    auto len = [&model](const char *support_type, const char *support_style, double spacing) {
        return support_interface_extrusion_length(slice({ model }, {
            { "enable_support", 1 }, { "layer_height", 0.2 }, { "support_on_build_plate_only", 0 },
            { "support_type", support_type }, { "support_style", support_style }, { "support_interface_top_layers", 0 },
            { "support_interface_bottom_layers", 6 }, { "support_bottom_interface_spacing", spacing } }));
    };
    REQUIRE(len(type, style, 0.0) > len(type, style, 4.0) * 1.5);
}

// Interface and base flows are identical in width and rate unless a separate support-interface
// filament is used, so density is the observable here, not flow.
TEST_CASE("Bottom-only support interface keeps the dense interface density", "[SupportMaterial]")
{
    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, print, {
        { "enable_support",                   1 },
        { "support_interface_top_layers",     0 },
        { "support_interface_bottom_layers",  6 },
        { "support_bottom_interface_spacing", 0.0 },  // solid: density resolves to 1.0
        { "support_base_pattern_spacing",     2.5 },  // sparse: density stays below 1.0
    });
    SupportParameters sp(*print.objects().front());
    REQUIRE(sp.bottom_interface_density > sp.support_density);
}
