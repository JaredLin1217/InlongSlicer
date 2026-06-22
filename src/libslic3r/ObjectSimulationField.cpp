#include "ObjectSimulationField.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {
namespace {

static float clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

static float clampf(float value, float min_value, float max_value)
{
    return std::max(min_value, std::min(max_value, value));
}

static float smoothstep01(float value)
{
    const float t = clamp01(value);
    return t * t * (3.0f - 2.0f * t);
}

static int thermal_resolution_id(WarpThermalResolution resolution)
{
    switch (resolution) {
    case WarpThermalResolution::Fast: return 0;
    case WarpThermalResolution::High: return 2;
    case WarpThermalResolution::Auto:
    default: return 1;
    }
}

static int object_simulation_role_id(ObjectSimulationRole role)
{
    switch (role) {
    case ObjectSimulationRole::Solid: return 1;
    case ObjectSimulationRole::Infill: return 2;
    case ObjectSimulationRole::Gap: return 3;
    case ObjectSimulationRole::OuterWall: return 4;
    case ObjectSimulationRole::InnerWall: return 5;
    case ObjectSimulationRole::Corner: return 6;
    case ObjectSimulationRole::Other:
    default: return 0;
    }
}

static float distance_2d(float ax, float ay, float bx, float by)
{
    const float dx = ax - bx;
    const float dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

static float object_simulation_layer_release(uint32_t layer_id, int max_layers)
{
    const int safe_layers = std::max(1, max_layers);
    const float progress = safe_layers <= 1 ? 0.0f :
        clamp01(static_cast<float>(layer_id) / static_cast<float>(std::max(1, safe_layers - 1)));
    return 1.0f - smoothstep01(progress);
}

static float object_simulation_edge_influence(const ObjectSimulationPathInput& input, float smooth_scale)
{
    if (!input.layer_bounds_valid)
        return 0.0f;

    const float width = std::max(0.1f, input.layer_max_x - input.layer_min_x);
    const float depth = std::max(0.1f, input.layer_max_y - input.layer_min_y);
    const float edge_distance = std::min(
        std::min(std::fabs(input.x - input.layer_min_x), std::fabs(input.layer_max_x - input.x)),
        std::min(std::fabs(input.y - input.layer_min_y), std::fabs(input.layer_max_y - input.y)));
    const float edge_band = std::clamp(std::min(width, depth) * 0.12f * smooth_scale, 5.0f, 22.0f);
    return smoothstep01((edge_band - edge_distance) / edge_band);
}

static float object_simulation_corner_influence(const ObjectSimulationPathInput& input, float smooth_scale)
{
    if (!input.layer_bounds_valid)
        return clamp01(input.corner_influence);

    const float width = std::max(0.1f, input.layer_max_x - input.layer_min_x);
    const float depth = std::max(0.1f, input.layer_max_y - input.layer_min_y);
    const float corners[4][2] = {
        { input.layer_min_x, input.layer_min_y },
        { input.layer_min_x, input.layer_max_y },
        { input.layer_max_x, input.layer_min_y },
        { input.layer_max_x, input.layer_max_y },
    };
    float corner_distance = std::numeric_limits<float>::max();
    for (const auto& corner : corners)
        corner_distance = std::min(corner_distance, distance_2d(input.x, input.y, corner[0], corner[1]));

    const float corner_radius = std::clamp(std::min(width, depth) * 0.16f * smooth_scale, 7.0f, 30.0f);
    const float bbox_corner = smoothstep01((corner_radius - corner_distance) / corner_radius);
    return std::max(bbox_corner, 0.65f * clamp01(input.corner_influence));
}

static float object_simulation_long_path_influence(float segment_length)
{
    return smoothstep01((segment_length - 35.0f) / 90.0f);
}

static float object_simulation_role_base(ObjectSimulationRole role, const ObjectSimulationPathInput& input,
                                         float release, float retained_heat)
{
    const float material = clampf(0.035f + 0.080f * clamp01(input.material_sensitivity) +
        (input.high_warp_material ? 0.030f : 0.0f), 0.035f, 0.130f);
    const float area = clamp01(input.layer_area_factor);
    const float retention_gradient = clamp01(release * (1.0f - 0.45f * clamp01(retained_heat)));
    float base = material + 0.065f * clamp01(input.environment_influence) + 0.050f * retention_gradient;

    if (input.has_brim)
        base -= 0.035f;
    if (input.has_raft)
        base -= 0.070f;

    switch (role) {
    case ObjectSimulationRole::Solid:
        base += 0.145f + 0.105f * area + 0.050f * release;
        break;
    case ObjectSimulationRole::Infill:
        base += 0.095f + 0.075f * area + 0.035f * release;
        break;
    case ObjectSimulationRole::Gap:
        base += 0.105f + 0.050f * area + 0.040f * release;
        break;
    case ObjectSimulationRole::OuterWall:
        base += 0.135f + 0.045f * area + 0.055f * release;
        break;
    case ObjectSimulationRole::InnerWall:
        base += 0.115f + 0.040f * area + 0.045f * release;
        break;
    case ObjectSimulationRole::Corner:
        base += 0.150f + 0.050f * area + 0.060f * release;
        break;
    case ObjectSimulationRole::Other:
    default:
        base += 0.075f + 0.025f * release;
        break;
    }

    return clampf(base, 0.02f, 0.55f);
}

} // namespace

ObjectSimulationResolutionProfile object_simulation_resolution_profile(WarpThermalResolution resolution)
{
    switch (resolution) {
    case WarpThermalResolution::Fast:
        return { 1.35f, 0.70f, 2 };
    case WarpThermalResolution::High:
        return { 0.85f, 1.10f, 6 };
    case WarpThermalResolution::Auto:
    default:
        return { 1.00f, 0.90f, 4 };
    }
}

float object_simulation_layer_coverage_heat(float object_length,
                                            float solid_length,
                                            float infill_length,
                                            float gap_length,
                                            float wall_length,
                                            float area)
{
    if (object_length <= 0.0f)
        return 0.0f;

    const float solid_ratio = clamp01((solid_length + 0.70f * infill_length + 0.45f * gap_length) /
        std::max(1.0f, object_length));
    const float wall_ratio = clamp01(wall_length / std::max(1.0f, object_length));
    const float size_factor = smoothstep01((std::sqrt(std::max(0.0f, area)) - 28.0f) / 110.0f);
    return clamp01(0.22f + 0.50f * solid_ratio + 0.14f * wall_ratio + 0.14f * size_factor);
}

float object_simulation_retained_heat_decay(int layer_offset, WarpThermalResolution resolution)
{
    if (layer_offset <= 0)
        return 0.0f;

    const ObjectSimulationResolutionProfile shape = object_simulation_resolution_profile(resolution);
    return std::exp(-static_cast<float>(layer_offset) /
        (1.30f + 0.55f * static_cast<float>(shape.retained_layers)));
}

float object_simulation_retained_heat_warmup(uint32_t layer_id, float body_coverage, WarpThermalResolution resolution)
{
    if (layer_id == 0)
        return 0.0f;

    const ObjectSimulationResolutionProfile shape = object_simulation_resolution_profile(resolution);
    const float warmup = 1.0f - std::exp(-static_cast<float>(layer_id) /
        (1.30f + 0.55f * static_cast<float>(shape.retained_layers)));
    return clamp01(clamp01(body_coverage) * warmup);
}

ObjectSimulationFieldSample sample_object_simulation_field(const ObjectSimulationFieldInput& input)
{
    ObjectSimulationFieldSample sample;
    sample.field_role = object_simulation_role_id(input.role);
    sample.field_resolution = thermal_resolution_id(input.resolution);
    if (sample.field_role == 0)
        return sample;

    const float material = clamp01(input.material_sensitivity);
    const float field_detail = clamp01(input.level_strength);
    const float layer_weight = clamp01(input.layer_weight);
    const float protection_weight = clamp01(input.protection_weight);
    const float edge = clamp01(input.edge_influence);
    const float corner = clamp01(input.corner_influence);
    const float long_path = clamp01(input.long_path_influence);
    const float area = clamp01(input.area_influence);
    const float environment = clamp01(input.environment_influence);
    const float thermal = clamp01(input.thermal_influence);
    const bool solid = input.role == ObjectSimulationRole::Solid;
    const bool infill = input.role == ObjectSimulationRole::Infill;
    const bool gap = input.role == ObjectSimulationRole::Gap;
    const bool model_fill = solid || infill || gap;
    const bool wall = input.role == ObjectSimulationRole::OuterWall || input.role == ObjectSimulationRole::InnerWall;
    const bool corner_role = input.role == ObjectSimulationRole::Corner;

    const float adhesion_relief = (input.has_raft ? 0.070f : 0.0f) + (input.has_brim ? 0.035f : 0.0f);
    const float material_bias = clampf(0.035f + 0.080f * material + (input.high_warp_material ? 0.030f : 0.0f), 0.035f, 0.130f);
    const float base_display = clampf(0.70f * clamp01(input.display_risk_base) + material_bias - adhesion_relief, 0.02f, model_fill ? 0.50f : 0.47f);

    const float stable_display_geometry = model_fill ?
        (0.012f * area + (infill ? 0.010f * long_path : 0.0f)) :
        (0.018f * long_path);
    const float geometry_display = model_fill ?
        (0.026f * edge + 0.018f * corner + stable_display_geometry) :
        (0.010f * edge + 0.070f * corner + stable_display_geometry);

    sample.display_risk = clamp01(base_display + geometry_display);

    const float base_process = model_fill ?
        clamp01(0.38f * sample.display_risk + 0.09f * protection_weight + 0.045f * layer_weight + 0.07f * environment + 0.012f * area) :
        clamp01(0.56f * sample.display_risk + 0.15f * protection_weight + 0.09f * layer_weight + 0.09f * environment);
    const float geometry_process = model_fill ?
        (0.980f * edge + 0.720f * corner + 0.030f * area + 0.095f * thermal) :
        (0.090f * edge + 0.180f * corner + 0.055f * long_path + 0.045f * thermal);
    sample.base_weight = clamp01(base_process);
    sample.local_weight = clamp01(geometry_process);
    sample.geometry_delta = geometry_process;
    sample.process_weight = clamp01(base_process + field_detail * geometry_process +
                                    (wall ? 0.030f : 0.0f) + (corner_role ? 0.050f : 0.0f));
    return sample;
}

ObjectSimulationFieldSample sample_object_simulation_path(const ObjectSimulationPathInput& input)
{
    if (input.role == ObjectSimulationRole::Other)
        return {};

    const ObjectSimulationResolutionProfile shape = object_simulation_resolution_profile(input.resolution);
    const float release = object_simulation_layer_release(input.layer_id, input.max_layers);
    const float edge = object_simulation_edge_influence(input, shape.smooth_scale);
    const float corner = object_simulation_corner_influence(input, shape.smooth_scale);
    const float long_path = object_simulation_long_path_influence(input.segment_length);
    const float base = object_simulation_role_base(input.role, input, release, input.retained_heat);

    ObjectSimulationFieldInput field;
    field.layer_id = input.layer_id;
    field.max_layers = std::max(1, input.max_layers);
    field.role = input.role;
    field.resolution = input.resolution;
    field.material_sensitivity = input.material_sensitivity;
    field.high_warp_material = input.high_warp_material;
    field.has_brim = input.has_brim;
    field.has_raft = input.has_raft;
    field.display_risk_base = base;
    field.layer_weight = release;
    field.protection_weight = release;
    field.edge_influence = edge;
    field.corner_influence = corner;
    field.long_path_influence = long_path;
    field.area_influence = clamp01(input.layer_area_factor);
    field.environment_influence = clamp01(input.environment_influence);
    field.thermal_influence = clamp01(0.55f * release + 0.45f * (1.0f - clamp01(input.retained_heat)));
    field.level_strength = input.level_strength > 0.0f ? input.level_strength : (0.56f + 0.16f * shape.detail_gain);
    return sample_object_simulation_field(field);
}

} // namespace Slic3r
