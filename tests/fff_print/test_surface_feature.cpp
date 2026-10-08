#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <vector>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "test_helpers.hpp"

using namespace Slic3r;

namespace {

// A narrow neck between two plates has a top shoulder at Z=2 and a
// bridge-bottom shoulder at Z=4. Neither endpoint needs enhancement.
TriangleMesh surface_feature_mesh()
{
    TriangleMesh mesh = make_cube(12, 12, 2);
    TriangleMesh neck = make_cube(6, 6, 2);
    neck.translate(3, 3, 2);
    mesh.merge(neck);
    TriangleMesh cap = make_cube(12, 12, 2);
    cap.translate(0, 0, 4);
    mesh.merge(cap);
    return mesh;
}

// A 1.4 mm U-shaped wall meets a 1.5 mm plate at Z=1.8 and a cap
// at Z=3.9 with 0.3 mm layers. Its paths lie inside existing plate-wall
// beads, but their centerlines differ. Bead coverage is not path identity.
TriangleMesh thin_wall_feature_mesh()
{
    TriangleMesh mesh = make_cube(20, 20, 1.5);
    TriangleMesh left = make_cube(1.4, 20, 2.1);
    left.translate(0, 0, 1.5);
    mesh.merge(left);
    TriangleMesh right = make_cube(1.4, 20, 2.1);
    right.translate(18.6, 0, 1.5);
    mesh.merge(right);
    TriangleMesh front = make_cube(17.2, 1.4, 2.1);
    front.translate(1.4, 0, 1.5);
    mesh.merge(front);
    TriangleMesh cap = make_cube(20, 20, 1.5);
    cap.translate(0, 0, 3.6);
    mesh.merge(cap);
    return mesh;
}

DynamicPrintConfig feature_config(const char *wall_generator, const char *vertical_shells,
                                 bool enabled, int top_layers, int bottom_layers)
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"wall_generator", wall_generator},
                                   {"ensure_vertical_shell_thickness", vertical_shells},
                                   {"layer_height", 0.2},
                                   {"initial_layer_print_height", 0.2},
                                   {"elefant_foot_compensation", 0},
                                   {"wall_loops", 3},
                                   {"only_one_wall_top", false},
                                   {"top_shell_layers", 3},
                                   {"bottom_shell_layers", 3},
                                   {"sparse_infill_density", "10%"},
                                   {"enable_support", false},
                                   {"surface_feature_enhance_mode", enabled},
                                   {"top_feature_embed_layers", top_layers},
                                   {"bottom_feature_extend_layers", bottom_layers}});
    return config;
}

double path_length(const ExtrusionEntityCollection &collection)
{
    double length = 0;
    const ExtrusionEntityCollection flat = collection.flatten(false);
    for (const ExtrusionEntity *entity : flat.entities)
        length += unscale<double>(entity->length());
    return length;
}

Polygons wall_coverage(const LayerRegion &region)
{
    Polygons result;
    region.perimeters.polygons_covered_by_width(result, float(SCALED_EPSILON));
    region.thin_fills.polygons_covered_by_width(result, float(SCALED_EPSILON));
    return union_(result);
}

double polygon_area_mm2(const Polygons &polygons)
{
    double result = 0;
    for (const ExPolygon &polygon : union_ex(polygons))
        result += polygon.area() * SCALING_FACTOR * SCALING_FACTOR;
    return result;
}

double polyline_length_mm(const Polylines &polylines)
{
    double length = 0.;
    for (const Polyline &polyline : polylines)
        length += unscale<double>(polyline.length());
    return length;
}

Polygons wall_centerlines(const LayerRegion &region, double tolerance)
{
    Polygons result;
    const auto collect = [&](const ExtrusionPath &path) {
        polygons_append(result, offset(path.polyline.to_polyline(), float(scale_(tolerance))));
    };
    for_each_extrusion_path(region.perimeters, collect);
    for_each_extrusion_path(region.thin_fills, collect);
    return union_(result);
}

std::vector<double> wall_lengths(const Print &print)
{
    std::vector<double> result;
    for (const Layer *layer : print.objects().front()->layers()) {
        double length = 0;
        for (const LayerRegion *region : layer->regions())
            length += path_length(region->perimeters) + path_length(region->thin_fills);
        result.emplace_back(length);
    }
    return result;
}

void check_same_paths(const Print &actual, const Print &expected)
{
    const auto actual_lengths = wall_lengths(actual);
    const auto expected_lengths = wall_lengths(expected);
    REQUIRE(actual_lengths.size() == expected_lengths.size());
    for (size_t i = 0; i < actual_lengths.size(); ++i) {
        CAPTURE(i);
        CHECK_THAT(actual_lengths[i], Catch::Matchers::WithinAbs(expected_lengths[i], 0.001));
        const LayerRegion &a = *actual.objects().front()->get_layer(int(i))->regions().front();
        const LayerRegion &b = *expected.objects().front()->get_layer(int(i))->regions().front();
        CHECK_THAT(polygon_area_mm2(diff(wall_coverage(a), wall_coverage(b))),
                   Catch::Matchers::WithinAbs(0., 0.0001));
        CHECK_THAT(polygon_area_mm2(diff(wall_coverage(b), wall_coverage(a))),
                   Catch::Matchers::WithinAbs(0., 0.0001));
        CHECK_THAT(path_length(a.fills), Catch::Matchers::WithinAbs(path_length(b.fills), 0.001));
    }
}

} // namespace

TEST_CASE("Surface feature contours align through thin walls even inside existing wall beads",
          "[SurfaceFeature][ThinWall][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const int top_layers = GENERATE(0, 1, 3);
    const int bottom_layers = GENERATE(0, 1, 3);
    CAPTURE(wall_generator, top_layers, bottom_layers);
    auto config = feature_config(wall_generator, "ensure_all", false, 3, 3);
    config.set_deserialize_strict({{"layer_height", 0.3}, {"initial_layer_print_height", 0.3},
                                   {"nozzle_diameter", "0.6"}, {"outer_wall_line_width", "102%"},
                                   {"inner_wall_line_width", "102%"}, {"only_one_wall_top", true}});
    Print baseline, enhanced;
    Model baseline_model, enhanced_model;
    Slic3r::Test::init_print({thin_wall_feature_mesh()}, baseline, baseline_model, config);
    config.set_deserialize_strict({{"surface_feature_enhance_mode", true},
                                   {"top_feature_embed_layers", top_layers},
                                   {"bottom_feature_extend_layers", bottom_layers}});
    Slic3r::Test::init_print({thin_wall_feature_mesh()}, enhanced, enhanced_model, config);
    baseline.process();
    enhanced.process();
    const PrintObject &original = *baseline.objects().front();
    const PrintObject &reinforced = *enhanced.objects().front();
    REQUIRE(original.layer_count() == 17);
    REQUIRE(reinforced.layer_count() == original.layer_count());
    for (const auto &[source_idx, target_idx, selected] :
         std::vector<std::tuple<int, int, bool>>{{5, 2, top_layers >= 3}, {5, 3, top_layers >= 2},
                                               {5, 4, top_layers >= 1}, {11, 12, bottom_layers >= 1},
                                               {11, 13, bottom_layers >= 2}, {11, 14, bottom_layers >= 3}}) {
        CAPTURE(source_idx, target_idx, selected);
        const LayerRegion &source = *original.get_layer(source_idx)->regions().front();
        const LayerRegion &before = *original.get_layer(target_idx)->regions().front();
        const LayerRegion &after = *reinforced.get_layer(target_idx)->regions().front();
        CHECK_THAT(reinforced.get_layer(target_idx)->print_z,
                   Catch::Matchers::WithinAbs((target_idx + 1) * 0.3, 0.000001));
        if (!selected) {
            CHECK_THAT(polygon_area_mm2(diff(wall_coverage(after), wall_coverage(before))),
                       Catch::Matchers::WithinAbs(0., 0.0001));
            CHECK_THAT(polygon_area_mm2(diff(wall_coverage(before), wall_coverage(after))),
                       Catch::Matchers::WithinAbs(0., 0.0001));
            continue;
        }
        const Polygons after_lines = wall_centerlines(after, 0.002);
        const Polygons before_lines = wall_centerlines(before, 0.002);
        const ExPolygons model_area = union_ex(to_expolygons(after.slices.surfaces));
        double expected_mm = 0., missing_before_mm = 0., missing_after_mm = 0.;
        Polygons expected_lines, expected_spacing;
        double endpoint_allowance = 0.;
        const auto check_projection = [&](const ExtrusionPath &path) {
            const Polygons allowed = to_polygons(offset_ex(model_area, -float(scale_(path.width * 0.5f))));
            const Polylines expected = intersection_pl(Polylines{path.polyline.to_polyline()}, allowed);
            expected_mm += polyline_length_mm(expected);
            missing_before_mm += polyline_length_mm(diff_pl(expected, before_lines));
            missing_after_mm += polyline_length_mm(diff_pl(expected, after_lines));
            endpoint_allowance += expected.size() * 4. * 0.002;
            for (const Polyline &line : expected) {
                polygons_append(expected_lines, offset(line, float(scale_(0.002))));
                ExtrusionPath projected(Polyline3(line), path);
                projected.height = float(reinforced.get_layer(target_idx)->height);
                projected.polygons_covered_by_spacing(expected_spacing, float(SCALED_EPSILON));
            }
        };
        for_each_extrusion_path(source.perimeters, check_projection);
        for_each_extrusion_path(source.thin_fills, check_projection);
        REQUIRE(expected_mm > 20.);
        REQUIRE(missing_before_mm > 1.);
        CHECK_THAT(missing_after_mm, Catch::Matchers::WithinAbs(0., 0.01));
        const Polygons added = diff(wall_coverage(after), wall_coverage(before));
        CHECK_THAT(polygon_area_mm2(diff(added, to_polygons(after.slices.surfaces))),
                   Catch::Matchers::WithinAbs(0., 0.001));
        expected_lines = union_(expected_lines);
        expected_spacing = union_(expected_spacing);
        const Polygons source_lines = wall_centerlines(source, 0.002);
        double conflicting_mm = 0., matching_mm = 0.;
        const auto check_conflicts = [&](const ExtrusionPath &path) {
            // A source end cap may lie exactly on the inset boundary: Clipper
            // excludes it from an intersection but retains it in a difference.
            // It is still a feature path, not a conflicting original wall.
            const Polylines line{path.polyline.to_polyline()};
            const Polylines non_feature = diff_pl(line, source_lines);
            matching_mm += polyline_length_mm(intersection_pl(line, expected_lines));
            const Flow flow = is_bridge(path.role()) ? Flow::bridging_flow(path.width, 0.f) :
                Flow(path.width, path.height, 0.f);
            conflicting_mm += polyline_length_mm(intersection_pl(non_feature,
                offset(expected_spacing, 0.5f * float(flow.scaled_spacing()))));
        };
        for_each_extrusion_path(after.perimeters, check_conflicts);
        for_each_extrusion_path(after.thin_fills, check_conflicts);
        CHECK_THAT(conflicting_mm, Catch::Matchers::WithinAbs(0., 0.01));
        CHECK(matching_mm <= expected_mm + endpoint_allowance + 0.001);
        const Polygons freed = intersection(diff(wall_coverage(before), wall_coverage(after)), to_polygons(after.slices.surfaces));
        CHECK_THAT(polygon_area_mm2(diff(freed, to_polygons(after.fill_surfaces.surfaces))),
                   Catch::Matchers::WithinAbs(0., 0.001));
        CHECK_THAT(polygon_area_mm2(diff(freed, to_polygons(after.fill_no_overlap_expolygons))),
                   Catch::Matchers::WithinAbs(0., 0.001));
        CHECK_THAT(polygon_area_mm2(diff(freed, to_polygons(after.fill_expolygons))),
                   Catch::Matchers::WithinAbs(0., 0.001));
    }
}

TEST_CASE("Surface feature embedding preserves alignment through elephant foot compensation",
          "[SurfaceFeature][ThinWall][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    auto config = feature_config(wall_generator, "ensure_all", false, 3, 0);
    config.set_deserialize_strict({{"layer_height", 0.3}, {"initial_layer_print_height", 0.3},
                                   {"nozzle_diameter", "0.6"}, {"outer_wall_line_width", "102%"},
                                   {"inner_wall_line_width", "102%"}, {"only_one_wall_top", true},
                                   {"elefant_foot_compensation", 0.1}, {"elefant_foot_compensation_layers", 3}});
    Print baseline, enhanced;
    Model baseline_model, enhanced_model;
    Slic3r::Test::init_print({thin_wall_feature_mesh()}, baseline, baseline_model, config);
    config.set_deserialize_strict({{"surface_feature_enhance_mode", true}});
    Slic3r::Test::init_print({thin_wall_feature_mesh()}, enhanced, enhanced_model, config);
    baseline.process();
    enhanced.process();
    const PrintObject &original = *baseline.objects().front();
    const PrintObject &reinforced = *enhanced.objects().front();
    REQUIRE(original.layer_count() == 17);
    REQUIRE(reinforced.layer_count() == original.layer_count());
    const LayerRegion &source = *original.get_layer(5)->regions().front();
    const Polygons source_lines = wall_centerlines(source, 0.002);
    std::map<float, Polygons> source_outer_lines;
    const auto collect_outer = [&](const ExtrusionPath &path) {
        if (path.role() == erExternalPerimeter)
            polygons_append(source_outer_lines[path.width], offset(path.polyline.to_polyline(), float(scale_(0.002))));
    };
    for_each_extrusion_path(source.perimeters, collect_outer);
    for (const int target_idx : {2, 3, 4}) {
        CAPTURE(wall_generator, target_idx);
        const Layer &layer = *reinforced.get_layer(target_idx);
        const LayerRegion &before = *original.get_layer(target_idx)->regions().front();
        const LayerRegion &after = *layer.regions().front();
        const Polygons after_lines = wall_centerlines(after, 0.002);
        double expected_mm = 0., missing_mm = 0.;
        const auto check_projection = [&](const ExtrusionPath &path) {
            const Polylines line{path.polyline.to_polyline()};
            expected_mm += polyline_length_mm(line);
            missing_mm += polyline_length_mm(diff_pl(line, after_lines));
        };
        for_each_extrusion_path(source.perimeters, check_projection);
        for_each_extrusion_path(source.thin_fills, check_projection);
        REQUIRE(expected_mm > 100.);
        CHECK_THAT(missing_mm, Catch::Matchers::WithinAbs(0., 0.01));
        const Polygons added = diff(wall_coverage(after), wall_coverage(before));
        CHECK_THAT(polygon_area_mm2(diff(added, to_polygons(after.slices.surfaces))),
                   Catch::Matchers::WithinAbs(0., 0.001));

        bool narrowed_feature = false;
        const auto check_flow = [&](const ExtrusionPath &path) {
            if (intersection_pl(Polylines{path.polyline.to_polyline()}, source_lines).empty())
                return;
            CHECK(path.width > layer.height * (1. - 0.25 * PI));
            CHECK_THAT(path.height, Catch::Matchers::WithinAbs(layer.height, 0.000001));
            CHECK_THAT(path.mm3_per_mm,
                       Catch::Matchers::WithinRel(Flow(path.width, float(layer.height), 0.f).mm3_per_mm(), 0.00001));
            if (path.role() == erExternalPerimeter)
                for (const auto &[width, lines] : source_outer_lines)
                    narrowed_feature |= path.width < width - 0.001f &&
                        polyline_length_mm(intersection_pl(Polylines{path.polyline.to_polyline()}, lines)) > 0.05;
        };
        for_each_extrusion_path(after.perimeters, check_flow);
        for_each_extrusion_path(after.thin_fills, check_flow);
        // Z=0.9 remains in the three-layer compensation window. Keep its
        // source centers with a narrower bead, not a clipped loop. Arachne
        // corners can also need local narrowing outside that window.
        if (target_idx == 2)
            CHECK(narrowed_feature);
        else if (std::string(wall_generator) == "classic")
            CHECK_FALSE(narrowed_feature);
    }
}

TEST_CASE("Surface feature layer counts reinforce only the selected top and bottom shoulders",
          "[SurfaceFeature][PrintObject][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const char *vertical_shells = GENERATE("ensure_all", "none");
    const int top_layers = GENERATE(0, 1, 3);
    const int bottom_layers = GENERATE(0, 1, 3);
    CAPTURE(wall_generator, vertical_shells, top_layers, bottom_layers);
    Print baseline, enhanced;
    Model baseline_model, enhanced_model;
    Slic3r::Test::init_print({surface_feature_mesh()}, baseline, baseline_model,
                           feature_config(wall_generator, vertical_shells, false, 3, 3));
    Slic3r::Test::init_print({surface_feature_mesh()}, enhanced, enhanced_model,
                           feature_config(wall_generator, vertical_shells, true, top_layers, bottom_layers));
    baseline.process();
    enhanced.process();
    const auto baseline_lengths = wall_lengths(baseline);
    const auto enhanced_lengths = wall_lengths(enhanced);
    REQUIRE(baseline_lengths.size() == 30);
    REQUIRE(enhanced_lengths.size() == baseline_lengths.size());
    for (int i = 0; i < int(baseline_lengths.size()); ++i) {
        CAPTURE(i);
        const bool top_target = i <= 9 && i > 9 - top_layers;
        const bool bottom_target = i >= 20 && i < 20 + bottom_layers;
        if (top_target || bottom_target)
            CHECK(enhanced_lengths[i] > baseline_lengths[i] + 1.);
        else
            CHECK_THAT(enhanced_lengths[i], Catch::Matchers::WithinAbs(baseline_lengths[i], 0.001));
    }
}

TEST_CASE("Zero surface feature layers leave walls and infill unchanged",
          "[SurfaceFeature][PrintObject][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    Print baseline, zero_layers;
    Model baseline_model, zero_layers_model;
    Slic3r::Test::init_print({surface_feature_mesh()}, baseline, baseline_model,
                           feature_config(wall_generator, "ensure_all", false, 3, 3));
    Slic3r::Test::init_print({surface_feature_mesh()}, zero_layers, zero_layers_model,
                           feature_config(wall_generator, "ensure_all", true, 0, 0));
    baseline.process();
    zero_layers.process();
    check_same_paths(zero_layers, baseline);
}

TEST_CASE("Surface feature paths stay inside the model and replace infill instead of doubling it",
          "[SurfaceFeature][PrintObject][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    Print baseline, enhanced;
    Model baseline_model, enhanced_model;
    Slic3r::Test::init_print({surface_feature_mesh()}, baseline, baseline_model,
                           feature_config(wall_generator, "ensure_all", false, 3, 3));
    Slic3r::Test::init_print({surface_feature_mesh()}, enhanced, enhanced_model,
                           feature_config(wall_generator, "ensure_all", true, 3, 3));
    baseline.process();
    enhanced.process();
    for (int i : {7, 8, 9, 20, 21, 22}) {
        CAPTURE(i);
        const LayerRegion &original = *baseline.objects().front()->get_layer(i)->regions().front();
        const LayerRegion &region = *enhanced.objects().front()->get_layer(i)->regions().front();
        const Polygons added = diff(wall_coverage(region), wall_coverage(original));
        REQUIRE(polygon_area_mm2(added) > 1.);
        CHECK_THAT(polygon_area_mm2(diff(added, to_polygons(region.slices.surfaces))),
                   Catch::Matchers::WithinAbs(0., 0.001));
        CHECK_THAT(polygon_area_mm2(intersection(added, to_polygons(region.fill_surfaces.surfaces))),
                   Catch::Matchers::WithinAbs(0., 0.001));
        CHECK_FALSE(region.fill_surfaces.empty());
    }
    // Enhancement must not relabel the real exterior shoulders as internal surfaces.
    const auto &top_slices = enhanced.objects().front()->get_layer(9)->regions().front()->slices.surfaces;
    const auto &bottom_slices = enhanced.objects().front()->get_layer(20)->regions().front()->slices.surfaces;
    CHECK(std::any_of(top_slices.begin(), top_slices.end(), [](const Surface &s) { return s.surface_type == stTop; }));
    CHECK(std::any_of(bottom_slices.begin(), bottom_slices.end(), [](const Surface &s) { return s.surface_type == stBottomBridge; }));
}

TEST_CASE("Changing surface enhancement settings regenerates the same paths as a fresh slice",
          "[SurfaceFeature][PrintObject][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    auto config = feature_config(wall_generator, "ensure_all", true, 3, 3);
    Print reused;
    Model reused_model;
    Slic3r::Test::init_print({surface_feature_mesh()}, reused, reused_model, config);
    reused.process();
    for (const auto &setting : std::vector<std::tuple<bool, int, int>>{{false, 3, 3}, {true, 0, 0}, {true, 1, 3}, {true, 3, 3}}) {
        const auto [enabled, top_layers, bottom_layers] = setting;
        CAPTURE(wall_generator, enabled, top_layers, bottom_layers);
        config.set_deserialize_strict({{"surface_feature_enhance_mode", enabled},
                                       {"top_feature_embed_layers", top_layers},
                                       {"bottom_feature_extend_layers", bottom_layers}});
        reused.apply(reused_model, config);
        reused.process();
        Print fresh;
        Model fresh_model;
        Slic3r::Test::init_print({surface_feature_mesh()}, fresh, fresh_model, config);
        fresh.process();
        check_same_paths(reused, fresh);
    }

    // An infill-only change also has to discard previously injected wall paths.
    config.set_deserialize_strict({{"sparse_infill_density", "20%"}});
    reused.apply(reused_model, config);
    CHECK_FALSE(reused.objects().front()->is_step_done(posPerimeters));
    CHECK(reused.objects().front()->is_step_done(posSlice));
    reused.process();
    Print fresh;
    Model fresh_model;
    Slic3r::Test::init_print({surface_feature_mesh()}, fresh, fresh_model, config);
    fresh.process();
    check_same_paths(reused, fresh);

    // Changing only shell preparation must regenerate the pristine walls too.
    config.set_deserialize_strict({{"top_shell_layers", 4}});
    reused.apply(reused_model, config);
    CHECK_FALSE(reused.objects().front()->is_step_done(posPerimeters));
    reused.process();
    Print fresh_shells;
    Model fresh_shells_model;
    Slic3r::Test::init_print({surface_feature_mesh()}, fresh_shells, fresh_shells_model, config);
    fresh_shells.process();
    check_same_paths(reused, fresh_shells);
    reused.process();
    check_same_paths(reused, fresh_shells);
}

TEST_CASE("Enhanced feature paths use the destination variable layer height and flow",
          "[SurfaceFeature][PrintObject][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    auto config = feature_config(wall_generator, "ensure_all", false, 3, 3);
    config.set_deserialize_strict({{"min_layer_height", "0.08"}, {"max_layer_height", "0.3"}});
    const auto baseline_config = config;
    Print baseline, enhanced;
    Model baseline_model, enhanced_model;
    Slic3r::Test::init_print({surface_feature_mesh()}, baseline, baseline_model, config);
    config.set_deserialize_strict({{"surface_feature_enhance_mode", true}});
    Slic3r::Test::init_print({surface_feature_mesh()}, enhanced, enhanced_model, config);
    const std::vector<coordf_t> profile{0., 0.2, 0.2, 0.2, 0.201, 0.1, 2., 0.1,
                                       2.001, 0.15, 4., 0.15, 4.001, 0.3, 6., 0.3};
    baseline_model.objects.front()->layer_height_profile.set(profile);
    enhanced_model.objects.front()->layer_height_profile.set(profile);
    baseline.apply(baseline_model, baseline_config);
    enhanced.apply(enhanced_model, config);
    baseline.process();
    enhanced.process();
    REQUIRE(baseline.objects().front()->layer_count() == enhanced.objects().front()->layer_count());
    size_t added_paths = 0;
    bool has_thin_layer = false;
    bool has_thick_layer = false;
    for (size_t i = 0; i < enhanced.objects().front()->layer_count(); ++i) {
        const Layer &layer = *enhanced.objects().front()->get_layer(int(i));
        const LayerRegion &region = *layer.regions().front();
        const Polygons original = wall_coverage(*baseline.objects().front()->get_layer(int(i))->regions().front());
        const auto check_flow = [&](const ExtrusionPath &path) {
            // Complete projected loops stay loops; inspect their paths too.
            if (diff_pl(Polylines{path.polyline.to_polyline()}, original).empty())
                return;
            CAPTURE(wall_generator, i, path.width, layer.height);
            ++added_paths;
            has_thin_layer |= layer.height < 0.15;
            has_thick_layer |= layer.height > 0.2;
            CHECK_THAT(path.height, Catch::Matchers::WithinAbs(layer.height, 0.000001));
            CHECK_THAT(path.mm3_per_mm,
                       Catch::Matchers::WithinRel(Flow(path.width, float(layer.height), 0.f).mm3_per_mm(), 0.00001));
        };
        for_each_extrusion_path(region.perimeters, check_flow);
        for_each_extrusion_path(region.thin_fills, check_flow);
    }
    CHECK(added_paths > 0);
    CHECK(has_thin_layer);
    CHECK(has_thick_layer);
}

TEST_CASE("Exported G-code contains the enhanced walls rather than only their settings",
          "[SurfaceFeature][GCode][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    auto config = feature_config(wall_generator, "ensure_all", false, 3, 3);
    Print baseline, enhanced;
    Model baseline_model, enhanced_model;
    Slic3r::Test::init_print({surface_feature_mesh()}, baseline, baseline_model, config);
    config.set_deserialize_strict({{"surface_feature_enhance_mode", true}});
    Slic3r::Test::init_print({surface_feature_mesh()}, enhanced, enhanced_model, config);

    auto exported_wall_lengths = [&config](const std::string &gcode) {
        std::map<int, double> lengths;
        ExtrusionRole role = erNone;
        GCodeReader reader;
        reader.apply_config(config);
        reader.parse_buffer(gcode, [&](GCodeReader &state, const GCodeReader::GCodeLine &line) {
            const std::string_view comment = line.comment();
            if (comment.substr(0, 5) == "TYPE:")
                role = ExtrusionEntity::string_to_role(comment.substr(5));
            if ((is_perimeter(role) || role == erGapFill) && line.extruding(state))
                lengths[int(std::round(state.z() * 1000.))] += line.dist_XY(state);
        });
        return lengths;
    };
    const auto before = exported_wall_lengths(Slic3r::Test::gcode(baseline));
    const auto after = exported_wall_lengths(Slic3r::Test::gcode(enhanced));
    for (const int z : {1600, 1800, 2000, 4200, 4400, 4600}) {
        CAPTURE(wall_generator, z);
        REQUIRE(before.count(z) == 1);
        REQUIRE(after.count(z) == 1);
        CHECK(after.at(z) > before.at(z) + 1.);
    }
}

TEST_CASE("Object overrides can disable surface enhancement without changing other objects",
          "[SurfaceFeature][PrintObject][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    Print baseline, enhanced;
    Model baseline_model, enhanced_model;
    Slic3r::Test::init_print({surface_feature_mesh(), surface_feature_mesh()}, baseline, baseline_model,
                           feature_config(wall_generator, "ensure_all", false, 3, 3));
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        {}, {{"surface_feature_enhance_mode", "0"}}};
    Slic3r::Test::init_print({surface_feature_mesh(), surface_feature_mesh()}, enhanced, enhanced_model,
                           feature_config(wall_generator, "ensure_all", true, 3, 3), &overrides);
    baseline.process();
    enhanced.process();
    REQUIRE(enhanced.objects().size() == 2);
    REQUIRE(baseline.objects().size() == 2);
    const LayerRegion &disabled = *enhanced.objects()[1]->get_layer(9)->regions().front();
    const LayerRegion &original = *baseline.objects()[1]->get_layer(9)->regions().front();
    CHECK_THAT(path_length(disabled.perimeters), Catch::Matchers::WithinAbs(path_length(original.perimeters), 0.001));
    CHECK_THAT(path_length(disabled.fills), Catch::Matchers::WithinAbs(path_length(original.fills), 0.001));
    const LayerRegion &enabled = *enhanced.objects()[0]->get_layer(9)->regions().front();
    CHECK(path_length(enabled.perimeters) > path_length(original.perimeters) + 1.);
}

TEST_CASE("Surface reinforcement preserves organic support geometry",
          "[SurfaceFeature][OrganicTree][Regression]")
{
    auto config = feature_config("classic", "ensure_all", false, 3, 3);
    config.set_deserialize_strict({{"enable_support", true},
                                   {"support_type", "tree(auto)"},
                                   {"support_style", "organic"},
                                   {"support_threshold_angle", 50},
                                   {"support_remove_small_overhang", false},
                                   {"support_top_z_distance", 0.2},
                                   {"support_bottom_z_distance", 0.2},
                                   {"support_object_xy_distance", 0.2}});
    Print baseline, enhanced;
    Model baseline_model, enhanced_model;
    Slic3r::Test::init_print({surface_feature_mesh()}, baseline, baseline_model, config);
    config.set_deserialize_strict({{"surface_feature_enhance_mode", true}});
    Slic3r::Test::init_print({surface_feature_mesh()}, enhanced, enhanced_model, config);
    baseline.process();
    enhanced.process();
    const PrintObject &a = *baseline.objects().front();
    const PrintObject &b = *enhanced.objects().front();
    REQUIRE(a.support_layer_count() > 0);
    REQUIRE(b.support_layer_count() == a.support_layer_count());
    for (size_t i = 0; i < a.support_layer_count(); ++i) {
        CAPTURE(i);
        const SupportLayer &original = *a.support_layers()[i];
        const SupportLayer &reinforced = *b.support_layers()[i];
        CHECK_THAT(reinforced.print_z, Catch::Matchers::WithinAbs(original.print_z, 0.000001));
        CHECK_THAT(path_length(reinforced.support_fills), Catch::Matchers::WithinAbs(path_length(original.support_fills), 0.0001));
        Polygons original_coverage, reinforced_coverage;
        original.support_fills.polygons_covered_by_width(original_coverage, float(SCALED_EPSILON));
        reinforced.support_fills.polygons_covered_by_width(reinforced_coverage, float(SCALED_EPSILON));
        CHECK_THAT(polygon_area_mm2(diff(reinforced_coverage, original_coverage)), Catch::Matchers::WithinAbs(0., 0.0001));
        CHECK_THAT(polygon_area_mm2(diff(original_coverage, reinforced_coverage)), Catch::Matchers::WithinAbs(0., 0.0001));
    }
}

TEST_CASE("Combined infill does not occupy enhanced walls on lower layers",
          "[SurfaceFeature][SurfaceInfill][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    auto config = feature_config(wall_generator, "ensure_all", false, 2, 2);
    config.set_deserialize_strict({{"layer_height", 0.1},
                                   {"initial_layer_print_height", 0.1},
                                   {"top_shell_layers", 0},
                                   {"bottom_shell_layers", 0},
                                   {"infill_combination", true}});
    Print baseline, enhanced;
    Model baseline_model, enhanced_model;
    Slic3r::Test::init_print({surface_feature_mesh()}, baseline, baseline_model, config);
    config.set_deserialize_strict({{"surface_feature_enhance_mode", true}});
    Slic3r::Test::init_print({surface_feature_mesh()}, enhanced, enhanced_model, config);
    baseline.process();
    enhanced.process();
    const PrintObject &object = *enhanced.objects().front();
    REQUIRE(object.layer_count() == baseline.objects().front()->layer_count());
    std::vector<Polygons> added_walls;
    for (size_t i = 0; i < object.layer_count(); ++i)
        added_walls.emplace_back(diff(wall_coverage(*object.get_layer(int(i))->regions().front()),
                                     wall_coverage(*baseline.objects().front()->get_layer(int(i))->regions().front())));
    size_t combined_surfaces = 0;
    bool lower_layer_enhancement = false;
    for (size_t i = 0; i < object.layer_count(); ++i)
        for (const Surface &surface : object.get_layer(int(i))->regions().front()->fill_surfaces.surfaces) {
            if (surface.thickness_layers <= 1 || surface.surface_type == stInternalVoid)
                continue;
            ++combined_surfaces;
            REQUIRE(size_t(surface.thickness_layers) <= i + 1);
            for (size_t j = i + 1 - surface.thickness_layers; j <= i; ++j) {
                CAPTURE(wall_generator, i, j);
                lower_layer_enhancement |= j < i && !added_walls[j].empty();
                CHECK_THAT(polygon_area_mm2(intersection(added_walls[j], to_polygons(surface.expolygon))),
                           Catch::Matchers::WithinAbs(0., 0.001));
            }
        }
    CHECK(combined_surfaces > 0);
    CHECK(lower_layer_enhancement);
}
