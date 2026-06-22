#ifndef slic3r_WarpPrevention_hpp_
#define slic3r_WarpPrevention_hpp_

#include "ObjectSimulationField.hpp"

#include <cstdint>
#include <string>

namespace Slic3r {

struct MaterialThermalProfile
{
    float conductivity_w_m_k{ 0.22f };
    float specific_heat_j_kg_k{ 1450.0f };
    float density_kg_m3{ 1250.0f };
    float thermal_expansion{ 75.0e-6f };
    float elastic_modulus_gpa{ 2.0f };
    float bonding_temperature{ 85.0f };
    float softening_temperature{ 85.0f };
    float shrinkage_percent{ 0.25f };
    float confidence{ 0.35f };
};

struct WarpPreventionMaterial
{
    float material_sensitivity{ 0.45f };
    float chamber_temperature{ 0.0f };
    float bed_temperature{ 0.0f };
    float shrinkage_delta{ 0.0f };
    float thermal_expansion{ 75.0e-6f };
    float elastic_modulus_gpa{ 2.0f };
    float softening_temperature{ 85.0f };
    float recommended_chamber_temperature{ 45.0f };
    float recommended_bed_temperature{ 80.0f };
    bool  high_warp_material{ false };
    bool  high_shrinkage{ false };
    bool  material_profile_known{ false };
    bool  shrinkage_configured{ false };
    MaterialThermalProfile thermal_profile;
};

struct WarpPreventionSegment
{
    uint32_t layer_id{ 0 };
    int      early_layers{ 15 };
    float    layer_height{ 0.0f };
    float    z{ 0.0f };
    float    length_mm{ 0.0f };
    float    elapsed_time_s{ 0.0f };
    float    x{ 0.0f };
    float    y{ 0.0f };
    float    x0{ 0.0f };
    float    y0{ 0.0f };
    float    x1{ 0.0f };
    float    y1{ 0.0f };
    float    path_corner_influence{ 0.0f };
    float    edge_proximity{ 0.0f };
    float    corner_proximity{ 0.0f };
    float    area_factor{ 1.0f };
    bool     layer_bounds_valid{ false };
    float    layer_min_x{ 0.0f };
    float    layer_min_y{ 0.0f };
    float    layer_max_x{ 0.0f };
    float    layer_max_y{ 0.0f };
    float    line_width{ 0.0f };
    float    flow_mm3_per_mm{ 0.0f };
    float    speed_mm_s{ 0.0f };
    float    acceleration_mm_s2{ 0.0f };
    float    role_acceleration_mm_s2{ 0.0f };
    float    nozzle_temperature{ 0.0f };
    float    bed_temperature{ 0.0f };
    float    chamber_temperature{ 0.0f };
    float    fan_speed{ 0.0f };
    bool     is_outer_wall{ false };
    bool     is_internal_wall{ false };
    bool     is_bottom_solid{ false };
    bool     is_model_infill{ false };
    bool     is_gap_fill{ false };
    bool     is_support{ false };
    bool     is_bridge{ false };
    bool     is_skirt_or_brim{ false };
    bool     is_wipe_tower{ false };
    bool     has_dedicated_acceleration_limit{ false };
    bool     has_brim{ false };
    bool     has_raft{ false };
    bool     has_inner_outer_brim{ false };
    float    brim_width{ 0.0f };
};

struct WarpPreventionSettings
{
    bool  enabled{ false };
    WarpThermalResolution thermal_resolution{ WarpThermalResolution::Auto };
    float max_slowdown_percent{ 40.0f };
    float min_wall_speed{ 14.0f };
    float min_bottom_speed{ 14.0f };
    int   early_layers{ 15 };
    bool  adjust_acceleration{ true };
    float max_accel_reduction_percent{ 30.0f };
    float min_wall_acceleration{ 1200.0f };
    float min_bottom_acceleration{ 1000.0f };
};

enum class WarpPreventionPresetLevel : uint8_t
{
    Conservative,
    Balanced,
    Aggressive,
};

struct WarpPreventionPreset
{
    float max_slowdown_percent{ 40.0f };
    float min_wall_speed{ 14.0f };
    float min_bottom_speed{ 14.0f };
    int   max_active_layers{ 15 };
    bool  adjust_acceleration{ true };
    float max_accel_reduction_percent{ 30.0f };
    float min_wall_acceleration{ 1200.0f };
    float min_bottom_acceleration{ 1000.0f };
};

WarpPreventionPreset warp_prevention_preset(WarpPreventionPresetLevel level);
WarpPreventionMaterial make_warp_prevention_material(const std::string& filament_type);

} // namespace Slic3r

#endif // slic3r_WarpPrevention_hpp_
