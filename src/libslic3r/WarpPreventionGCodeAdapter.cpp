#include "WarpPreventionGCodeAdapter.hpp"

#include "GCodeWriter.hpp"
#include "PrintConfig.hpp"
#include "format.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace Slic3r {

namespace {

static WarpThermalResolution to_warp_thermal_resolution(WarpPreventionThermalResolution resolution)
{
    switch (resolution) {
    case WarpPreventionThermalResolution::Fast: return WarpThermalResolution::Fast;
    case WarpPreventionThermalResolution::High: return WarpThermalResolution::High;
    case WarpPreventionThermalResolution::Auto:
    default: return WarpThermalResolution::Auto;
    }
}

static bool is_model_body_adjustable(const WarpPreventionSegment& segment)
{
    return segment.is_outer_wall || segment.is_internal_wall ||
        segment.is_bottom_solid || segment.is_model_infill || segment.is_gap_fill;
}

static float clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

static float smoothstep01(float value)
{
    const float t = clamp01(value);
    return t * t * (3.0f - 2.0f * t);
}

static float distance_2d(float ax, float ay, float bx, float by)
{
    const float dx = ax - bx;
    const float dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

static double apply_soft_lower_bound(double original, double target, double lower_bound,
                                     float process_weight, double min_visible_band,
                                     double max_visible_band, double ratio_visible_band)
{
    if (lower_bound <= 0.0 || target >= lower_bound)
        return target;
    if (original <= lower_bound)
        return original;

    const double headroom = std::max(0.0, original - lower_bound);
    if (headroom <= 0.0)
        return lower_bound;

    const double desired_band = std::clamp(original * ratio_visible_band, min_visible_band, max_visible_band);
    const double band = std::min(headroom, desired_band);
    const double shaped = lower_bound + band * (1.0 - static_cast<double>(clamp01(process_weight)));
    return std::clamp(shaped, lower_bound, original);
}

static float output_layer_envelope(uint32_t layer_id, int max_layers)
{
    if (max_layers <= 0 || layer_id >= static_cast<uint32_t>(max_layers))
        return 0.0f;
    if (max_layers == 1)
        return layer_id == 0 ? 1.0f : 0.0f;

    // The object simulation field describes risk.  The G-code output also needs
    // a process envelope so the last protected layer gently releases to normal
    // printing instead of stopping abruptly at max active layers.
    const float progress = clamp01(static_cast<float>(layer_id) / static_cast<float>(max_layers));
    return 1.0f - smoothstep01(progress);
}

static ObjectSimulationRole object_simulation_role_for(const WarpPreventionSegment& segment)
{
    if (segment.is_gap_fill)
        return ObjectSimulationRole::Gap;
    if (segment.is_model_infill)
        return ObjectSimulationRole::Infill;
    if (segment.is_bottom_solid)
        return ObjectSimulationRole::Solid;
    if (segment.is_outer_wall || segment.is_internal_wall) {
        const float corner = std::max(segment.path_corner_influence, segment.corner_proximity);
        if (corner > 0.18f)
            return ObjectSimulationRole::Corner;
        return segment.is_outer_wall ? ObjectSimulationRole::OuterWall : ObjectSimulationRole::InnerWall;
    }
    return ObjectSimulationRole::Other;
}

static float environment_influence_for(const WarpPreventionMaterial& material, const WarpPreventionSegment& segment)
{
    const float bed = segment.bed_temperature > 0.0f ? segment.bed_temperature : material.bed_temperature;
    const float chamber = segment.chamber_temperature > 0.0f ? segment.chamber_temperature : material.chamber_temperature;
    const float bed_deficit = material.recommended_bed_temperature > 0.0f ?
        clamp01((material.recommended_bed_temperature - bed) / 45.0f) : 0.0f;
    const float chamber_deficit = material.recommended_chamber_temperature > 0.0f ?
        clamp01((material.recommended_chamber_temperature - chamber) / 45.0f) : 0.0f;
    return clamp01(0.55f * bed_deficit + 0.45f * chamber_deficit + 0.20f * clamp01(segment.fan_speed / 100.0f));
}

static ObjectSimulationFieldSample sample_process_field(const WarpPreventionMaterial& material,
                                                        const WarpPreventionSegment& segment,
                                                        const WarpPreventionSettings& settings)
{
    const ObjectSimulationRole role = object_simulation_role_for(segment);
    if (role == ObjectSimulationRole::Other)
        return {};

    ObjectSimulationPathInput input;
    input.layer_id = segment.layer_id;
    input.max_layers = std::max(0, settings.early_layers);
    input.role = role;
    input.resolution = settings.thermal_resolution;
    input.material_sensitivity = material.material_sensitivity;
    input.high_warp_material = material.high_warp_material;
    input.has_brim = segment.has_brim;
    input.has_raft = segment.has_raft;
    input.environment_influence = environment_influence_for(material, segment);
    // Keep the shared simulation risk independent in the preview, but scale
    // output-only local detail by the selected protection strength.  Conservative
    // mode should not amplify short corner/edge solid segments into visible
    // speed steps; balanced/aggressive retain progressively more local shaping.
    const float level_strength = clamp01(settings.max_slowdown_percent / 60.0f);
    const float shaped_level_strength = level_strength * level_strength;
    input.level_strength = clamp01(shaped_level_strength * shaped_level_strength);
    if (segment.layer_id > 0) {
        const float body_coverage = clamp01(0.24f +
            (segment.is_bottom_solid ? 0.52f : 0.0f) +
            (segment.is_model_infill ? 0.38f : 0.0f) +
            (segment.is_gap_fill ? 0.28f : 0.0f) +
            ((segment.is_outer_wall || segment.is_internal_wall) ? 0.18f : 0.0f) +
            0.16f * clamp01(segment.area_factor));
        input.retained_heat = object_simulation_retained_heat_warmup(segment.layer_id, body_coverage, settings.thermal_resolution);
    }
    input.x = segment.x;
    input.y = segment.y;
    input.segment_length = segment.length_mm;
    input.corner_influence = std::max(segment.path_corner_influence, segment.corner_proximity);
    input.layer_bounds_valid = segment.layer_bounds_valid;
    input.layer_min_x = segment.layer_min_x;
    input.layer_min_y = segment.layer_min_y;
    input.layer_max_x = segment.layer_max_x;
    input.layer_max_y = segment.layer_max_y;
    input.layer_area_factor = clamp01(segment.area_factor);
    ObjectSimulationFieldSample sample = sample_object_simulation_path(input);
    const float layer_envelope = output_layer_envelope(segment.layer_id, settings.early_layers);
    sample.process_weight = clamp01(sample.process_weight * layer_envelope);
    sample.base_weight = clamp01(sample.base_weight * layer_envelope);
    sample.local_weight = clamp01(sample.local_weight * layer_envelope);
    sample.geometry_delta = clamp01(sample.geometry_delta * layer_envelope);
    return sample;
}

} // namespace

void WarpPreventionGCodeAdapter::reset_print()
{
    m_fan_output = FanOutputState{};
    m_last_set_acceleration = 0;
    m_available = false;
}

bool WarpPreventionGCodeAdapter::begin_path(const WarpPreventionPathContext& context, unsigned int initial_acceleration)
{
    m_available = false;
    m_role = context.role;
    m_last_set_acceleration = initial_acceleration;

    if (context.config == nullptr || !context.config->enable_warp_prevention.value)
        return false;

    const PrintConfig& config = *context.config;
    m_settings = WarpPreventionSettings{};
    m_settings.enabled = true;
    m_settings.thermal_resolution = to_warp_thermal_resolution(config.warp_prevention_thermal_resolution.value);
    m_settings.max_slowdown_percent = static_cast<float>(config.warp_prevention_max_slowdown.value);
    m_settings.min_wall_speed = static_cast<float>(config.warp_prevention_min_wall_speed.value);
    m_settings.min_bottom_speed = static_cast<float>(config.warp_prevention_min_bottom_speed.value);
    m_settings.early_layers = std::max(0, config.warp_prevention_early_layers.value);
    m_settings.adjust_acceleration = config.warp_prevention_adjust_acceleration.value;
    m_settings.max_accel_reduction_percent = static_cast<float>(config.warp_prevention_max_accel_reduction.value);
    m_settings.min_wall_acceleration = static_cast<float>(config.warp_prevention_min_wall_acceleration.value);
    m_settings.min_bottom_acceleration = static_cast<float>(config.warp_prevention_min_bottom_acceleration.value);

    m_material = make_warp_prevention_material(context.filament_type);
    m_material.chamber_temperature = context.chamber_temperature;
    m_material.bed_temperature = context.bed_temperature;
    if (std::abs(context.filament_shrink_percent - 100.0f) > 0.5f) {
        m_material.shrinkage_configured = true;
        m_material.shrinkage_delta = std::abs(context.filament_shrink_percent - 100.0f);
        m_material.thermal_profile.shrinkage_percent = m_material.shrinkage_delta;
        m_material.high_shrinkage = m_material.high_shrinkage || m_material.shrinkage_delta >= 0.8f;
    }

    m_base_segment = WarpPreventionSegment{};
    m_base_segment.layer_id = context.layer_id;
    m_base_segment.early_layers = m_settings.early_layers;
    m_base_segment.layer_height = context.layer_height;
    m_base_segment.line_width = context.line_width;
    m_base_segment.flow_mm3_per_mm = context.flow_mm3_per_mm;
    m_base_segment.z = context.z;
    m_base_segment.length_mm = context.path_length_mm;
    m_base_segment.elapsed_time_s = 0.0f;
    m_base_segment.speed_mm_s = std::max(0.0f, context.speed_mm_s);
    m_base_segment.acceleration_mm_s2 = context.acceleration_mm_s2;
    m_base_segment.role_acceleration_mm_s2 = context.acceleration_mm_s2;
    m_base_segment.x = context.path_x;
    m_base_segment.y = context.path_y;
    m_base_segment.path_corner_influence = 0.0f;
    m_base_segment.layer_bounds_valid = context.layer_bounds_valid;
    m_base_segment.layer_min_x = context.layer_min_x;
    m_base_segment.layer_min_y = context.layer_min_y;
    m_base_segment.layer_max_x = context.layer_max_x;
    m_base_segment.layer_max_y = context.layer_max_y;
    m_base_segment.nozzle_temperature = context.nozzle_temperature;
    m_base_segment.bed_temperature = m_material.bed_temperature;
    m_base_segment.chamber_temperature = m_material.chamber_temperature;
    m_base_segment.fan_speed = context.fan_speed;
    m_base_segment.is_outer_wall = context.role == erExternalPerimeter || context.role == erOverhangPerimeter;
    m_base_segment.is_internal_wall = is_internal_perimeter(context.role);
    m_base_segment.is_bottom_solid =
        context.role == erBottomSurface || context.role == erSolidInfill || context.role == erTopSolidInfill;
    m_base_segment.is_model_infill = context.role == erInternalInfill;
    m_base_segment.is_gap_fill = context.role == erGapFill;
    const bool model_fill = m_base_segment.is_bottom_solid || m_base_segment.is_model_infill || m_base_segment.is_gap_fill;
    m_base_segment.edge_proximity = m_base_segment.is_outer_wall ? 1.0f : (model_fill ? 0.35f : 0.0f);
    m_base_segment.corner_proximity = 0.0f;
    m_base_segment.area_factor = m_base_segment.is_bottom_solid ? 1.0f :
        (m_base_segment.is_model_infill ? 0.65f : (m_base_segment.is_gap_fill ? 0.45f : 0.0f));
    m_base_segment.is_support = context.role == erSupportMaterial || context.role == erSupportMaterialInterface || context.role == erSupportTransition;
    m_base_segment.is_bridge = context.role == erBridgeInfill ||
        context.role == erInternalBridgeInfill ||
        context.role == erOverhangPerimeter;
    m_base_segment.is_skirt_or_brim = context.role == erSkirt || context.role == erBrim;
    m_base_segment.is_wipe_tower = context.role == erWipeTower;
    m_base_segment.has_dedicated_acceleration_limit = context.role == erBridgeInfill ||
        context.role == erInternalBridgeInfill || context.role == erOverhangPerimeter;
    m_base_segment.has_brim = context.has_brim;
    m_base_segment.has_raft = context.has_raft;
    m_base_segment.has_inner_outer_brim = context.has_inner_outer_brim;
    m_base_segment.brim_width = context.brim_width;

    m_available = true;
    return true;
}

WarpPreventionSegment WarpPreventionGCodeAdapter::make_subsegment(double ax, double ay, double bx, double by,
                                                                  double length_mm, float corner_influence) const
{
    return make_subsegment(ax, ay, bx, by, length_mm, corner_influence, corner_influence);
}

WarpPreventionSegment WarpPreventionGCodeAdapter::make_subsegment(double ax, double ay, double bx, double by,
                                                                  double length_mm, float corner_influence,
                                                                  float corner_proximity) const
{
    WarpPreventionSegment segment = m_base_segment;
    segment.length_mm = static_cast<float>(std::max(0.0, length_mm));
    segment.elapsed_time_s = 0.0f;
    segment.x = static_cast<float>(0.5 * (ax + bx));
    segment.y = static_cast<float>(0.5 * (ay + by));
    segment.x0 = static_cast<float>(ax);
    segment.y0 = static_cast<float>(ay);
    segment.x1 = static_cast<float>(bx);
    segment.y1 = static_cast<float>(by);
    segment.path_corner_influence = std::clamp(corner_influence, 0.0f, 1.0f);
    segment.corner_proximity = std::clamp(corner_proximity, 0.0f, 1.0f);
    if (segment.layer_bounds_valid) {
        const float width = std::max(0.1f, segment.layer_max_x - segment.layer_min_x);
        const float depth = std::max(0.1f, segment.layer_max_y - segment.layer_min_y);
        const float edge_distance = std::min(
            std::min(std::fabs(segment.x - segment.layer_min_x), std::fabs(segment.layer_max_x - segment.x)),
            std::min(std::fabs(segment.y - segment.layer_min_y), std::fabs(segment.layer_max_y - segment.y)));
        const float edge_band = std::clamp(std::min(width, depth) * 0.12f, 5.0f, 22.0f);
        const float edge = smoothstep01((edge_band - edge_distance) / edge_band);

        const std::array<std::array<float, 2>, 4> corners{ {
            { segment.layer_min_x, segment.layer_min_y },
            { segment.layer_min_x, segment.layer_max_y },
            { segment.layer_max_x, segment.layer_min_y },
            { segment.layer_max_x, segment.layer_max_y }
        } };
        float corner_distance = std::numeric_limits<float>::max();
        for (const auto& c : corners)
            corner_distance = std::min(corner_distance, distance_2d(segment.x, segment.y, c[0], c[1]));
        const float corner_radius = std::clamp(std::min(width, depth) * 0.16f, 7.0f, 30.0f);
        const float bbox_corner = smoothstep01((corner_radius - corner_distance) / corner_radius);

        if (segment.is_outer_wall)
            segment.edge_proximity = std::max(segment.edge_proximity, 1.0f);
        else
            segment.edge_proximity = std::max(segment.edge_proximity, edge);
        segment.corner_proximity = std::max(segment.corner_proximity, bbox_corner);
    }
    return segment;
}

float WarpPreventionGCodeAdapter::segment_corner_influence(double prev_x, double prev_y,
                                                           double current_x, double current_y,
                                                           double next_x, double next_y) const
{
    const double in_x = current_x - prev_x;
    const double in_y = current_y - prev_y;
    const double out_x = next_x - current_x;
    const double out_y = next_y - current_y;
    const double in_len = std::sqrt(in_x * in_x + in_y * in_y);
    const double out_len = std::sqrt(out_x * out_x + out_y * out_y);
    if (in_len <= 1e-9 || out_len <= 1e-9)
        return 0.0f;

    const double dot = std::clamp((in_x * out_x + in_y * out_y) / (in_len * out_len), -1.0, 1.0);
    const double angle = std::acos(dot);
    return static_cast<float>(std::clamp((angle - 0.35) / 1.15, 0.0, 1.0));
}

float WarpPreventionGCodeAdapter::corner_influence_at(double distance_from_start, double length_mm,
                                                      float start_corner, float end_corner,
                                                      double path_width) const
{
    if (length_mm <= 1e-9)
        return std::max(start_corner, end_corner);

    const double corner_window = std::clamp(path_width * 18.0, 8.0, 14.0);
    const double window = std::min(0.5 * length_mm, corner_window);
    if (window <= 1e-9)
        return std::max(start_corner, end_corner);

    const float start_influence = start_corner > 0.0f ?
        static_cast<float>(start_corner * smoothstep01(static_cast<float>(1.0 - distance_from_start / window))) : 0.0f;
    const float end_influence = end_corner > 0.0f ?
        static_cast<float>(end_corner * smoothstep01(static_cast<float>(1.0 - (length_mm - distance_from_start) / window))) : 0.0f;
    return std::clamp(std::max(start_influence, end_influence), 0.0f, 1.0f);
}

std::vector<double> WarpPreventionGCodeAdapter::corner_split_params(double length_mm,
                                                                    float start_turn,
                                                                    float end_turn,
                                                                    bool split_wall_corners,
                                                                    double path_width) const
{
    (void) length_mm;
    (void) start_turn;
    (void) end_turn;
    (void) split_wall_corners;
    (void) path_width;
    return { 0.0, 1.0 };
}

std::vector<double> WarpPreventionGCodeAdapter::process_field_split_params(double length_mm,
                                                                           bool split_process_field,
                                                                           double path_width) const
{
    (void) length_mm;
    (void) split_process_field;
    (void) path_width;
    return { 0.0, 1.0 };
}

WarpPreventionSegmentDecision WarpPreventionGCodeAdapter::process_subsegment(double original_speed_mm_s,
                                                                             unsigned int original_acceleration,
                                                                             double ax, double ay,
                                                                             double bx, double by,
                                                                             double length_mm,
                                                                             float corner_influence,
                                                                             float corner_proximity,
                                                                             GCodeWriter& writer,
                                                                             double jerk)
{
    return process_segment(
        original_speed_mm_s,
        original_acceleration,
        make_subsegment(ax, ay, bx, by, length_mm, corner_influence, corner_proximity),
        writer,
        jerk);
}

WarpPreventionSegmentDecision WarpPreventionGCodeAdapter::process_segment(double original_speed_mm_s,
                                                                          unsigned int original_acceleration,
                                                                          const WarpPreventionSegment& segment,
                                                                          GCodeWriter& writer,
                                                                          double jerk)
{
    WarpPreventionSegmentDecision decision;
    decision.segment = segment;
    decision.final_speed_mm_s = original_speed_mm_s;
    decision.final_acceleration_mm_s2 = original_acceleration;

    ProcessState state;
    decision.final_speed_mm_s = cap_speed(original_speed_mm_s, segment, &state);
    decision.final_acceleration_mm_s2 = cap_acceleration(original_acceleration, segment, state);
    decision.acceleration_gcode = acceleration_gcode_if_needed(writer, decision.final_acceleration_mm_s2, jerk);
    decision.fan_gcode = fan_gcode_if_needed(writer, state, segment);

    return decision;
}

bool WarpPreventionGCodeAdapter::can_adjust_speed(const WarpPreventionSegment& segment) const
{
    if (!m_available || !m_settings.enabled)
        return false;
    if (m_settings.early_layers <= 0 || segment.layer_id >= static_cast<uint32_t>(m_settings.early_layers))
        return false;
    if (segment.is_support || segment.is_bridge || segment.is_skirt_or_brim || segment.is_wipe_tower)
        return false;
    if (m_role == erOverhangPerimeter || m_role == erInternalBridgeInfill)
        return false;
    return is_model_body_adjustable(segment);
}

bool WarpPreventionGCodeAdapter::can_adjust_fan(const WarpPreventionSegment& segment) const
{
    if (!m_available || !m_settings.enabled)
        return false;
    if (m_settings.early_layers <= 0 || segment.layer_id >= static_cast<uint32_t>(m_settings.early_layers))
        return false;
    if (segment.is_support || segment.is_bridge || segment.is_skirt_or_brim || segment.is_wipe_tower)
        return false;
    if (m_role == erOverhangPerimeter || m_role == erInternalBridgeInfill)
        return false;
    return is_model_body_adjustable(segment);
}

bool WarpPreventionGCodeAdapter::can_adjust_accel(const WarpPreventionSegment& segment) const
{
    if (!m_available || !m_settings.enabled || !m_settings.adjust_acceleration)
        return false;
    if (m_settings.early_layers <= 0 || segment.layer_id >= static_cast<uint32_t>(m_settings.early_layers))
        return false;
    if (segment.is_support || segment.is_bridge || segment.is_skirt_or_brim || segment.is_wipe_tower)
        return false;
    if (segment.has_dedicated_acceleration_limit || m_role == erOverhangPerimeter || m_role == erInternalBridgeInfill)
        return false;
    return is_model_body_adjustable(segment);
}

double WarpPreventionGCodeAdapter::cap_speed(double candidate_speed, const WarpPreventionSegment& source_segment,
                                             ProcessState* out_state)
{
    if (!m_available || candidate_speed <= 0.0)
        return candidate_speed;

    WarpPreventionSegment segment = source_segment;
    segment.speed_mm_s = static_cast<float>(candidate_speed);
    segment.elapsed_time_s = 0.0f;
    const ObjectSimulationFieldSample sample = sample_process_field(m_material, segment, m_settings);
    double capped_speed = candidate_speed;
    const float process_weight = clamp01(sample.process_weight);
    if (can_adjust_speed(segment) && process_weight > 0.0001f) {
        const float max_slowdown = std::clamp(m_settings.max_slowdown_percent, 0.0f, 60.0f) / 100.0f;
        capped_speed = candidate_speed * (1.0 - static_cast<double>(max_slowdown * process_weight));
        double lower_bound = 0.0;
        if ((segment.is_outer_wall || segment.is_internal_wall) && m_settings.min_wall_speed > 0.0f)
            lower_bound = std::max(lower_bound, static_cast<double>(m_settings.min_wall_speed));
        if ((segment.is_bottom_solid || segment.is_model_infill || segment.is_gap_fill) && m_settings.min_bottom_speed > 0.0f)
            lower_bound = std::max(lower_bound, static_cast<double>(m_settings.min_bottom_speed));
        // Keep the safety floor, but preserve enough room for the Object Simulation
        // field to remain visible in the exported feedrate.  A very narrow band
        // collapses first-layer solid areas into a single preview color.
        const double strength = std::clamp(static_cast<double>(m_settings.max_slowdown_percent) / 60.0, 0.0, 1.0);
        const double shaped_strength = strength * strength;
        const double visible_min = 0.08 + 0.34 * shaped_strength;
        const double visible_max = 0.24 + 0.96 * shaped_strength;
        const double visible_ratio = 0.012 + 0.054 * shaped_strength;
        capped_speed = apply_soft_lower_bound(candidate_speed, capped_speed, lower_bound, process_weight,
                                              visible_min, visible_max, visible_ratio);
        capped_speed = std::min(candidate_speed, capped_speed);
    }
    if (out_state != nullptr)
        out_state->process_weight = process_weight;
    if (out_state != nullptr)
        out_state->local_weight = clamp01(sample.local_weight);
    return capped_speed;
}

unsigned int WarpPreventionGCodeAdapter::cap_acceleration(unsigned int original_accel,
                                                          const WarpPreventionSegment& source_segment,
                                                          const ProcessState& state)
{
    if (!m_available || original_accel == 0 || !can_adjust_accel(source_segment))
        return original_accel;

    WarpPreventionSegment segment = source_segment;
    segment.acceleration_mm_s2 = static_cast<float>(original_accel);
    segment.role_acceleration_mm_s2 = source_segment.role_acceleration_mm_s2 > 0.0f ?
        source_segment.role_acceleration_mm_s2 : static_cast<float>(original_accel);
    const float accel_weight = clamp01(state.process_weight);
    double capped_accel = static_cast<double>(original_accel);
    if (accel_weight > 0.0001f) {
        const float max_reduction = std::clamp(m_settings.max_accel_reduction_percent, 0.0f, 90.0f) / 100.0f;
        capped_accel = static_cast<double>(original_accel) * (1.0 - static_cast<double>(max_reduction * accel_weight));
        double lower_bound = 0.0;
        if ((segment.is_outer_wall || segment.is_internal_wall) && m_settings.min_wall_acceleration > 0.0f)
            lower_bound = std::max(lower_bound, static_cast<double>(m_settings.min_wall_acceleration));
        if ((segment.is_bottom_solid || segment.is_model_infill || segment.is_gap_fill) && m_settings.min_bottom_acceleration > 0.0f)
            lower_bound = std::max(lower_bound, static_cast<double>(m_settings.min_bottom_acceleration));
        // Acceleration needs a wider visible band than speed, otherwise the
        // standard acceleration preview stays visually flat even though the
        // process field is changing.
        capped_accel = apply_soft_lower_bound(static_cast<double>(original_accel), capped_accel, lower_bound, accel_weight,
                                              1500.0, 7200.0, 0.64);
    }
    capped_accel = std::min(static_cast<double>(original_accel), capped_accel);
    return static_cast<unsigned int>(std::max(1.0, std::floor(capped_accel + 0.5)));
}

std::string WarpPreventionGCodeAdapter::fan_gcode_if_needed(GCodeWriter& writer,
                                                            const ProcessState& state,
                                                            const WarpPreventionSegment& segment)
{
    if (!m_available)
        return {};

    const double base_fan = std::clamp(static_cast<double>(segment.fan_speed), 0.0, 100.0);
    double target_fan = base_fan;
    const float process_weight = clamp01(state.process_weight);
    if (can_adjust_fan(segment) && base_fan > 0.0 && process_weight > 0.0001f) {
        const double max_fan_reduction = 0.35;
        target_fan = base_fan * (1.0 - max_fan_reduction * static_cast<double>(process_weight));
        target_fan = std::clamp(target_fan, base_fan * 0.65, base_fan);
    }

    if (!m_fan_output.valid) {
        m_fan_output.valid = true;
        m_fan_output.current_fan_speed = base_fan;
    }

    const double actual_fan = target_fan;
    const bool fan_will_output = std::lround(m_fan_output.current_fan_speed) != std::lround(actual_fan);

    std::string gcode;
    if (fan_will_output)
        gcode += writer.set_fan(static_cast<unsigned int>(std::lround(actual_fan)));
    m_fan_output.current_fan_speed = actual_fan;

    return gcode;
}

std::string WarpPreventionGCodeAdapter::acceleration_gcode_if_needed(GCodeWriter& writer,
                                                                     unsigned int target_accel,
                                                                     double jerk)
{
    if (target_accel == 0 || target_accel == m_last_set_acceleration)
        return {};

    std::string gcode;
    if (writer.get_gcode_flavor() == gcfKlipper)
        gcode += writer.set_accel_and_jerk(target_accel, jerk);
    else
        gcode += writer.set_print_acceleration(target_accel);
    m_last_set_acceleration = target_accel;
    return gcode;
}

} // namespace Slic3r
