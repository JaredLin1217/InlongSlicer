#ifndef slic3r_ObjectSimulationField_hpp_
#define slic3r_ObjectSimulationField_hpp_

#include <cstdint>

namespace Slic3r {

enum class WarpThermalResolution : uint8_t
{
    Fast,
    Auto,
    High,
};

enum class ObjectSimulationRole : uint8_t
{
    Other,
    Solid,
    Infill,
    Gap,
    OuterWall,
    InnerWall,
    Corner,
};

struct ObjectSimulationResolutionProfile
{
    float smooth_scale{ 1.0f };
    float detail_gain{ 1.0f };
    int retained_layers{ 4 };
};

struct ObjectSimulationFieldInput
{
    uint32_t layer_id{ 0 };
    int max_layers{ 0 };
    ObjectSimulationRole role{ ObjectSimulationRole::Other };
    WarpThermalResolution resolution{ WarpThermalResolution::Auto };
    float material_sensitivity{ 0.45f };
    bool high_warp_material{ false };
    bool has_brim{ false };
    bool has_raft{ false };
    float display_risk_base{ 0.0f };
    float layer_weight{ 0.0f };
    float protection_weight{ 0.0f };
    float edge_influence{ 0.0f };
    float corner_influence{ 0.0f };
    float long_path_influence{ 0.0f };
    float area_influence{ 0.0f };
    float environment_influence{ 0.0f };
    float thermal_influence{ 0.0f };
    float level_strength{ 0.0f };
};

struct ObjectSimulationPathInput
{
    uint32_t layer_id{ 0 };
    int max_layers{ 0 };
    ObjectSimulationRole role{ ObjectSimulationRole::Other };
    WarpThermalResolution resolution{ WarpThermalResolution::Auto };
    float material_sensitivity{ 0.45f };
    bool high_warp_material{ false };
    bool has_brim{ false };
    bool has_raft{ false };
    float environment_influence{ 0.0f };
    float level_strength{ 0.0f };
    float retained_heat{ 0.0f };
    float x{ 0.0f };
    float y{ 0.0f };
    float segment_length{ 0.0f };
    float corner_influence{ 0.0f };
    bool layer_bounds_valid{ false };
    float layer_min_x{ 0.0f };
    float layer_min_y{ 0.0f };
    float layer_max_x{ 0.0f };
    float layer_max_y{ 0.0f };
    float layer_area_factor{ 0.0f };
};

struct ObjectSimulationFieldSample
{
    float display_risk{ 0.0f };
    float process_weight{ 0.0f };
    float base_weight{ 0.0f };
    float local_weight{ 0.0f };
    float geometry_delta{ 0.0f };
    int field_role{ 0 };
    int field_resolution{ 0 };
};

ObjectSimulationResolutionProfile object_simulation_resolution_profile(WarpThermalResolution resolution);
float object_simulation_layer_coverage_heat(float object_length,
                                            float solid_length,
                                            float infill_length,
                                            float gap_length,
                                            float wall_length,
                                            float area);
float object_simulation_retained_heat_decay(int layer_offset, WarpThermalResolution resolution);
float object_simulation_retained_heat_warmup(uint32_t layer_id, float body_coverage, WarpThermalResolution resolution);
ObjectSimulationFieldSample sample_object_simulation_field(const ObjectSimulationFieldInput& input);
ObjectSimulationFieldSample sample_object_simulation_path(const ObjectSimulationPathInput& input);

} // namespace Slic3r

#endif // slic3r_ObjectSimulationField_hpp_
