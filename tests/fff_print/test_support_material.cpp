#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCode/ConflictChecker.hpp"
#include "libslic3r/Fill/FillBase.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Support/SupportCommon.hpp"
#include "libslic3r/Support/TreeSupportUtils.hpp"

#include "test_data.hpp" // get access to init_print, etc
#include "test_utils.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <tuple>

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

} // namespace

TEST_CASE("SupportMaterial: Three raft layers created", "[SupportMaterial]")
{
	Slic3r::Print print;
	Slic3r::Test::init_and_process_print({ TestMesh::cube_20x20x20 }, print, {
        { "enable_support", 1 },
        { "raft_layers",    3 }
		});
    REQUIRE(print.objects().front()->support_layers().size() == 3);
}

TEST_CASE("SupportMaterial: enforced support layers are generated", "[SupportMaterial]")
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

SCENARIO("SupportMaterial: support_layers_z and contact_distance", "[SupportMaterial]")
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
