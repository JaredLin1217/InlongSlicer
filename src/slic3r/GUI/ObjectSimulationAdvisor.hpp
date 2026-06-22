#ifndef slic3r_GUI_ObjectSimulationAdvisor_hpp_
#define slic3r_GUI_ObjectSimulationAdvisor_hpp_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace libvgcode {
struct GCodeInputData;
class Viewer;
struct PathVertex;
}

namespace Slic3r {

class DynamicPrintConfig;
class Print;

namespace GUI {

enum class ObjectSimulationAdviceSeverity : uint8_t
{
    Info,
    Low,
    Medium,
    High
};

enum class ObjectSimulationAdviceCategory : uint8_t
{
    Cooling,
    Chamber,
    Bed,
    Adhesion,
    Geometry,
    Speed,
    Material,
    ThermalModel
};

enum class ObjectSimulationConfigScope : uint8_t
{
    None,
    Print,
    Filament
};

enum class ObjectSimulationConfigValueType : uint8_t
{
    Float,
    Int,
    Bool,
    FloatVector,
    IntVector,
    BrimType,
    WarpPreventionLevel,
    WarpPreventionThermalResolution
};

struct ObjectSimulationConfigChange
{
    ObjectSimulationConfigScope     scope{ ObjectSimulationConfigScope::None };
    std::string             key;
    ObjectSimulationConfigValueType value_type{ ObjectSimulationConfigValueType::Float };
    double                  float_value{ 0.0 };
    int                     int_value{ 0 };
};

struct ObjectSimulationAdvice
{
    ObjectSimulationAdviceSeverity          severity{ ObjectSimulationAdviceSeverity::Info };
    ObjectSimulationAdviceCategory          category{ ObjectSimulationAdviceCategory::ThermalModel };
    uint32_t                        reason_flags{ 0 };
    std::vector<std::string>        target_config_keys;
    std::vector<ObjectSimulationConfigChange> changes;
    std::string                     current_value;
    std::string                     recommended_value;
    std::string                     message;
    std::string                     detail;
    bool                            requires_reslice{ true };
    bool                            can_apply{ false };
    float                           expected_risk_delta{ 0.0f };
    float                           confidence{ 0.0f };
};

struct ObjectSimulationOptimizationSummary
{
    bool                     valid{ false };
    float                    model_highest_risk{ 0.0f };
    float                    visible_highest_risk{ 0.0f };
    uint32_t                 model_reason_flags{ 0 };
    uint32_t                 visible_reason_flags{ 0 };
    std::array<int, 3>       visible_risk_counts{ 0, 0, 0 };
    std::vector<ObjectSimulationAdvice> advice;
};

struct ObjectSimulationAdviceView
{
    ObjectSimulationAdvice advice;
    bool would_change{ false };
};

struct ObjectSimulationAdvisorConfig
{
    bool        valid{ false };
    std::string material_family;
    float       material_sensitivity{ 0.45f };
    bool        high_warp_material{ false };
    bool        material_profile_known{ false };
    bool        has_brim{ false };
    bool        has_raft{ false };
    float       recommended_chamber_temperature{ 45.0f };
    float       recommended_bed_temperature{ 80.0f };
    float       temperature_vitrification{ 0.0f };
    float       chamber_temperature{ 0.0f };
    float       hot_plate_temp{ 0.0f };
    float       hot_plate_temp_initial_layer{ 0.0f };
    float       fan_min_speed{ 20.0f };
    float       fan_max_speed{ 100.0f };
    float       first_x_layer_fan_speed{ 0.0f };
    float       additional_cooling_fan_speed{ 0.0f };
    float       outer_wall_speed{ 0.0f };
    float       inner_wall_speed{ 0.0f };
    float       initial_layer_speed{ 0.0f };
    bool        warp_prevention_enabled{ false };
    int         warp_prevention_thermal_resolution{ 1 };
    float       warp_prevention_max_slowdown{ 40.0f };
    float       warp_prevention_min_wall_speed{ 14.0f };
    float       warp_prevention_min_bottom_speed{ 14.0f };
    int         warp_prevention_early_layers{ 15 };
    bool        warp_prevention_adjust_acceleration{ true };
    float       warp_prevention_max_accel_reduction{ 30.0f };
    float       warp_prevention_min_wall_acceleration{ 1200.0f };
    float       warp_prevention_min_bottom_acceleration{ 1000.0f };
    float       brim_width{ 0.0f };
    int         brim_type{ 0 };
    float       brim_ears_max_angle{ 125.0f };
    float       brim_ears_detection_length{ 1.0f };
    int         raft_layers{ 0 };
    int         extruder_id{ 0 };
};

struct ObjectSimulationRuntimeConfigState
{
    ObjectSimulationAdvisorConfig config;
    bool current_warp_prevention_enabled{ false };
    bool sliced_config_valid{ false };
    bool sliced_warp_prevention_enabled{ false };
    bool pending_reslice{ false };
    bool warp_layers_disabled{ false };
};

struct ObjectSimulationPreviewSummary
{
    bool found{ false };
    size_t max_vertex_index{ 0 };
    uint32_t reason_flags{ 0 };
    int low_count{ 0 };
    int medium_count{ 0 };
    int high_count{ 0 };
    int extrusion_count{ 0 };
    float material_confidence_sum{ 0.0f };
    float simulation_confidence_sum{ 0.0f };
    int confidence_count{ 0 };
};

struct ObjectSimulationPanelText
{
    std::string current_risk;
    std::string next_step;
    std::string processing_status;
    std::string added_time;
    std::string remaining_risk;
    std::vector<std::string> useful_adjustments;
};

struct ObjectSimulationTextRow
{
    std::string label;
    std::string value;
};

ObjectSimulationAdvisorConfig make_object_simulation_advisor_config(const Print& print);
ObjectSimulationRuntimeConfigState make_object_simulation_runtime_config_state(
    const ObjectSimulationAdvisorConfig& sliced_config,
    const DynamicPrintConfig* current_print_config);
ObjectSimulationPreviewSummary make_object_simulation_preview_summary(const libvgcode::Viewer& viewer);
ObjectSimulationPanelText make_object_simulation_panel_text(
    const libvgcode::PathVertex& max_vertex,
    const ObjectSimulationPreviewSummary& preview_summary,
    const ObjectSimulationRuntimeConfigState& runtime_config);
std::vector<ObjectSimulationTextRow> make_object_simulation_detail_rows(
    const libvgcode::PathVertex& max_vertex,
    const ObjectSimulationPreviewSummary& preview_summary,
    const ObjectSimulationRuntimeConfigState& runtime_config,
    const ObjectSimulationPanelText& panel_text);
std::vector<ObjectSimulationAdviceView> make_object_simulation_advice_view(
    const ObjectSimulationOptimizationSummary& optimization,
    const DynamicPrintConfig* print_config,
    const DynamicPrintConfig* filament_config);
void assign_object_simulation(libvgcode::GCodeInputData& data, const Print& print);
void clear_object_simulation(libvgcode::GCodeInputData& data);
ObjectSimulationOptimizationSummary make_object_simulation_optimization_summary(const libvgcode::Viewer& viewer, const ObjectSimulationAdvisorConfig& config);
bool apply_object_simulation_advice(const ObjectSimulationAdvice& advice, DynamicPrintConfig& print_config, DynamicPrintConfig& filament_config);
bool object_simulation_advice_changes_config(
    const ObjectSimulationAdvice& advice,
    const DynamicPrintConfig& print_config,
    const DynamicPrintConfig& filament_config);
bool prepare_object_simulation_advice_config_change(
    const ObjectSimulationAdvice& advice,
    const DynamicPrintConfig& print_config,
    const DynamicPrintConfig& filament_config,
    DynamicPrintConfig& next_print_config,
    DynamicPrintConfig& next_filament_config,
    bool& print_changed,
    bool& filament_changed);
bool apply_object_simulation_advice_to_current_plater(const ObjectSimulationAdvice& advice);
std::string object_simulation_advice_severity_label(ObjectSimulationAdviceSeverity severity);
std::string object_simulation_thermal_resolution_label(int resolution);
std::string object_simulation_config_key_label(const std::string& key);
std::string object_simulation_config_keys_label(const std::vector<std::string>& keys);
std::string object_simulation_reasons_to_string(uint32_t reasons);
float preview_effective_object_simulation(const libvgcode::PathVertex& vertex);
std::string object_simulation_level_label(float risk);
std::string object_simulation_remaining_reason_label(uint32_t reasons);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ObjectSimulationAdvisor_hpp_
