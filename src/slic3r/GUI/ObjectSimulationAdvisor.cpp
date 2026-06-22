#include "ObjectSimulationAdvisor.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "ObjectSimulationAnalysis.hpp"
#include "Plater.hpp"
#include "Tab.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/WarpPrevention.hpp"

#include <libvgcode/include/GCodeInputData.hpp>
#include <libvgcode/include/PathVertex.hpp>
#include <libvgcode/include/Types.hpp>
#include <libvgcode/include/Viewer.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_map>

namespace Slic3r {
namespace GUI {

namespace {

static constexpr size_t MAX_SCAN_VERTICES = 150000;

static float clampf(float value, float low, float high)
{
    return std::max(low, std::min(value, high));
}

static std::string uppercase_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

static bool has_token(const std::string& text, const std::string& token)
{
    return text.find(token) != std::string::npos;
}

static std::string format_temperature(float value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(0) << value << " C";
    return out.str();
}

static std::string format_percent(float value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(0) << value << " %";
    return out.str();
}

static std::string format_speed(float value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(0) << value << " mm/s";
    return out.str();
}

static std::string object_simulation_role_label(libvgcode::EGCodeExtrusionRole role)
{
    using Role = libvgcode::EGCodeExtrusionRole;
    switch (role) {
    case Role::None:                     return _u8L("Unknown");
    case Role::Perimeter:                return _u8L("Inner wall");
    case Role::ExternalPerimeter:        return _u8L("Outer wall");
    case Role::OverhangPerimeter:        return _u8L("Overhang wall");
    case Role::InternalInfill:           return _u8L("Sparse infill");
    case Role::SolidInfill:              return _u8L("Internal solid infill");
    case Role::TopSolidInfill:           return _u8L("Top surface");
    case Role::Ironing:                  return _u8L("Ironing");
    case Role::BridgeInfill:             return _u8L("Bridge");
    case Role::GapFill:                  return _u8L("Gap infill");
    case Role::Skirt:                    return _u8L("Skirt");
    case Role::SupportMaterial:          return _u8L("Support");
    case Role::SupportMaterialInterface: return _u8L("Support interface");
    case Role::WipeTower:                return _u8L("Prime tower");
    case Role::Custom:                   return _u8L("Custom");
    case Role::COUNT:                    return "";
    case Role::BottomSurface:            return _u8L("Bottom surface");
    case Role::InternalBridgeInfill:     return _u8L("Internal bridge");
    case Role::Brim:                     return _u8L("Brim");
    case Role::SupportTransition:        return _u8L("Support transition");
    case Role::Mixed:                    return _u8L("Mixed");
    }
    return _u8L("Unknown");
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

static WarpThermalResolution to_warp_thermal_resolution(WarpPreventionThermalResolution resolution);

static float segment_length(const libvgcode::PathVertex& a, const libvgcode::PathVertex& b)
{
    const float dx = b.position[0] - a.position[0];
    const float dy = b.position[1] - a.position[1];
    const float dz = b.position[2] - a.position[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct ObjectSimulationExtruderProfile
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

struct ObjectSimulationProfile
{
    std::vector<ObjectSimulationExtruderProfile> extruders;
    bool has_brim{ false };
    bool has_raft{ false };
    bool has_inner_outer_brim{ false };
    float brim_width{ 0.0f };
    bool warp_prevention_enabled{ false };
    WarpThermalResolution warp_prevention_thermal_resolution{ WarpThermalResolution::Auto };
    float warp_prevention_max_slowdown{ 40.0f };
    float warp_prevention_min_wall_speed{ 14.0f };
    float warp_prevention_min_bottom_speed{ 14.0f };
    int warp_prevention_early_layers{ 15 };
    bool warp_prevention_adjust_acceleration{ true };
    float warp_prevention_max_accel_reduction{ 30.0f };
    float warp_prevention_min_wall_acceleration{ 1200.0f };
    float warp_prevention_min_bottom_acceleration{ 1000.0f };
};

constexpr float object_simulation_advice_threshold = 0.38f;

static ObjectSimulationExtruderProfile default_object_simulation_extruder_profile()
{
    return ObjectSimulationExtruderProfile{};
}

static void apply_object_simulation_material_profile(const std::string& filament_type, ObjectSimulationExtruderProfile& profile)
{
    const WarpPreventionMaterial material = make_warp_prevention_material(filament_type);
    profile.material_sensitivity = material.material_sensitivity;
    profile.shrinkage_delta = material.shrinkage_delta;
    profile.thermal_expansion = material.thermal_expansion;
    profile.elastic_modulus_gpa = material.elastic_modulus_gpa;
    profile.softening_temperature = material.softening_temperature;
    profile.recommended_chamber_temperature = material.recommended_chamber_temperature;
    profile.recommended_bed_temperature = material.recommended_bed_temperature;
    profile.high_warp_material = material.high_warp_material;
    profile.high_shrinkage = material.high_shrinkage;
    profile.material_profile_known = material.material_profile_known;
    profile.shrinkage_configured = material.shrinkage_configured;
    profile.thermal_profile = material.thermal_profile;
}

static ObjectSimulationProfile make_object_simulation_profile(const Print& print)
{
    ObjectSimulationProfile profile;
    const PrintConfig& cfg = print.config();
    const auto* filament_types = cfg.option<ConfigOptionStrings>("filament_type");
    const auto* chambers       = cfg.option<ConfigOptionInts>("chamber_temperature");
    const auto* beds           = cfg.option<ConfigOptionInts>("hot_plate_temp");
    const auto* first_beds     = cfg.option<ConfigOptionInts>("hot_plate_temp_initial_layer");
    const auto* shrinkages     = cfg.option<ConfigOptionPercents>("filament_shrink");

    const size_t extruder_count = std::max<size_t>(1, filament_types != nullptr ? filament_types->values.size() : 0);
    profile.extruders.resize(extruder_count, default_object_simulation_extruder_profile());
    profile.has_brim = print.has_brim();
    if (const auto* brim_width = cfg.option<ConfigOptionFloat>("brim_width"); brim_width != nullptr)
        profile.brim_width = static_cast<float>(brim_width->value);
    if (const auto* brim = cfg.option<ConfigOptionEnum<BrimType>>("brim_type"); brim != nullptr)
        profile.has_inner_outer_brim = brim->value == btOuterAndInner;
    const auto objects = print.objects();
    profile.has_raft = std::any_of(objects.begin(), objects.end(), [](const PrintObject* object) {
        return object != nullptr && object->has_raft();
    });
    profile.warp_prevention_enabled = cfg.enable_warp_prevention.value;
    profile.warp_prevention_thermal_resolution = to_warp_thermal_resolution(cfg.warp_prevention_thermal_resolution.value);
    profile.warp_prevention_max_slowdown = static_cast<float>(cfg.warp_prevention_max_slowdown.value);
    profile.warp_prevention_min_wall_speed = static_cast<float>(cfg.warp_prevention_min_wall_speed.value);
    profile.warp_prevention_min_bottom_speed = static_cast<float>(cfg.warp_prevention_min_bottom_speed.value);
    profile.warp_prevention_early_layers = std::max(0, cfg.warp_prevention_early_layers.value);
    profile.warp_prevention_adjust_acceleration = cfg.warp_prevention_adjust_acceleration.value;
    profile.warp_prevention_max_accel_reduction = static_cast<float>(cfg.warp_prevention_max_accel_reduction.value);
    profile.warp_prevention_min_wall_acceleration = static_cast<float>(cfg.warp_prevention_min_wall_acceleration.value);
    profile.warp_prevention_min_bottom_acceleration = static_cast<float>(cfg.warp_prevention_min_bottom_acceleration.value);

    for (size_t i = 0; i < profile.extruders.size(); ++i) {
        ObjectSimulationExtruderProfile& extruder = profile.extruders[i];
        const std::string filament_type = filament_types != nullptr ? filament_types->get_at(i) : std::string();
        apply_object_simulation_material_profile(filament_type, extruder);
        extruder.chamber_temperature = chambers != nullptr ? static_cast<float>(chambers->get_at(i)) : 0.0f;
        const float bed_temp = beds != nullptr ? static_cast<float>(beds->get_at(i)) : 0.0f;
        const float first_bed_temp = first_beds != nullptr ? static_cast<float>(first_beds->get_at(i)) : bed_temp;
        extruder.bed_temperature = std::max(bed_temp, first_bed_temp);
        if (shrinkages != nullptr) {
            extruder.shrinkage_configured = true;
            extruder.shrinkage_delta = std::fabs(static_cast<float>(shrinkages->get_at(i)) - 100.0f);
            extruder.thermal_profile.shrinkage_percent = extruder.shrinkage_delta;
        }
        extruder.high_shrinkage = extruder.shrinkage_delta > 0.5f;
    }

    return profile;
}

static const ObjectSimulationExtruderProfile& warp_profile_for_extruder(const ObjectSimulationProfile& profile, uint8_t extruder_id)
{
    static const ObjectSimulationExtruderProfile fallback = default_object_simulation_extruder_profile();
    return extruder_id < profile.extruders.size() ? profile.extruders[extruder_id] : fallback;
}

static WarpPreventionMaterial to_warp_prevention_material(const ObjectSimulationExtruderProfile& profile)
{
    WarpPreventionMaterial material;
    material.material_sensitivity = profile.material_sensitivity;
    material.chamber_temperature = profile.chamber_temperature;
    material.bed_temperature = profile.bed_temperature;
    material.shrinkage_delta = profile.shrinkage_delta;
    material.thermal_expansion = profile.thermal_expansion;
    material.elastic_modulus_gpa = profile.elastic_modulus_gpa;
    material.softening_temperature = profile.softening_temperature;
    material.recommended_chamber_temperature = profile.recommended_chamber_temperature;
    material.recommended_bed_temperature = profile.recommended_bed_temperature;
    material.high_warp_material = profile.high_warp_material;
    material.high_shrinkage = profile.high_shrinkage;
    material.material_profile_known = profile.material_profile_known;
    material.shrinkage_configured = profile.shrinkage_configured;
    material.thermal_profile = profile.thermal_profile;
    if (material.shrinkage_configured)
        material.thermal_profile.shrinkage_percent = material.shrinkage_delta;
    return material;
}

static void assign_object_simulation(libvgcode::GCodeInputData& data, const ObjectSimulationProfile& profile)
{
    for (libvgcode::PathVertex& vertex : data.vertices) {
        vertex.object_simulation = 0.0f;
        vertex.object_simulation_reasons = libvgcode::ObjectSimulationNone;
        vertex.object_simulation_confidence = 0.0f;
        vertex.object_simulation_material_confidence = 0.0f;
        vertex.object_simulation_model_confidence = 0.0f;

        if (!vertex.is_extrusion())
            continue;

        const ObjectSimulationExtruderProfile& extruder = warp_profile_for_extruder(profile, vertex.extruder_id);
        const float bed_temp = vertex.bed_temperature > 0.0f ? vertex.bed_temperature : extruder.bed_temperature;
        const float chamber_temp = vertex.chamber_temperature > 0.0f ? vertex.chamber_temperature : extruder.chamber_temperature;
        vertex.bed_temperature = bed_temp;
        vertex.chamber_temperature = chamber_temp;

        float confidence = 0.48f;
        confidence += extruder.material_profile_known ? 0.18f : 0.0f;
        confidence += extruder.shrinkage_configured ? 0.08f : 0.0f;
        confidence += vertex.temperature > 0.0f ? 0.08f : 0.0f;
        confidence += bed_temp > 0.0f ? 0.08f : 0.0f;
        confidence += chamber_temp > 0.0f ? 0.06f : 0.0f;
        vertex.object_simulation_confidence = clamp01(confidence);
        vertex.object_simulation_material_confidence = clamp01(extruder.material_profile_known ? 0.88f : 0.55f);
        vertex.object_simulation_model_confidence = clamp01(0.72f + (profile.warp_prevention_thermal_resolution == WarpThermalResolution::High ? 0.16f : 0.08f));
    }

    ObjectSimulationConfig object_simulation_config;
    object_simulation_config.analysis_layers = std::max(1, profile.warp_prevention_early_layers);
    object_simulation_config.thermal_resolution = profile.warp_prevention_thermal_resolution;
    object_simulation_config.has_brim = profile.has_brim;
    object_simulation_config.has_raft = profile.has_raft;
    if (!profile.extruders.empty()) {
        float material_sum = 0.0f;
        float environment_sum = 0.0f;
        bool high_warp_material = false;
        for (const ObjectSimulationExtruderProfile& extruder : profile.extruders) {
            material_sum += extruder.material_sensitivity;
            high_warp_material = high_warp_material || extruder.high_warp_material;
            const float chamber_env = extruder.chamber_temperature > 0.0f ? extruder.chamber_temperature : 25.0f;
            const float chamber_deficit = extruder.recommended_chamber_temperature > 0.0f ?
                clamp01((extruder.recommended_chamber_temperature - chamber_env) / extruder.recommended_chamber_temperature) : 0.0f;
            const float bed_deficit = extruder.recommended_bed_temperature > 0.0f ?
                clamp01((extruder.recommended_bed_temperature - extruder.bed_temperature) / extruder.recommended_bed_temperature) : 0.0f;
            environment_sum += clamp01(0.70f * chamber_deficit + 0.30f * bed_deficit);
        }
        object_simulation_config.material_sensitivity = material_sum / static_cast<float>(profile.extruders.size());
        object_simulation_config.environment_influence = environment_sum / static_cast<float>(profile.extruders.size());
        object_simulation_config.high_warp_material = high_warp_material;
    }

    assign_object_simulation_display_risk(data, object_simulation_config);

    for (libvgcode::PathVertex& vertex : data.vertices) {
        if (!vertex.is_extrusion())
            continue;

        const ObjectSimulationExtruderProfile& extruder = warp_profile_for_extruder(profile, vertex.extruder_id);
        uint32_t reasons = libvgcode::ObjectSimulationNone;
        if (vertex.object_simulation > 0.01f) {
            if (extruder.high_warp_material)
                reasons |= libvgcode::ObjectSimulationHighShrinkMaterial;
            if (vertex.layer_id == 0 || vertex.layer_id < static_cast<uint32_t>(std::max(1, profile.warp_prevention_early_layers)))
                reasons |= libvgcode::ObjectSimulationBottomLayer;
            if (vertex.role == libvgcode::EGCodeExtrusionRole::ExternalPerimeter ||
                vertex.role == libvgcode::EGCodeExtrusionRole::OverhangPerimeter)
                reasons |= libvgcode::ObjectSimulationOuterWall;
            if (vertex.role == libvgcode::EGCodeExtrusionRole::BottomSurface ||
                vertex.role == libvgcode::EGCodeExtrusionRole::SolidInfill ||
                vertex.role == libvgcode::EGCodeExtrusionRole::TopSolidInfill ||
                vertex.role == libvgcode::EGCodeExtrusionRole::InternalInfill)
                reasons |= libvgcode::ObjectSimulationLargeBottomArea;
            if (profile.has_brim || profile.has_raft)
                reasons |= libvgcode::ObjectSimulationBrimRaftProtected;
        }
        vertex.object_simulation_reasons = reasons;
    }
}

} // namespace


void assign_object_simulation(libvgcode::GCodeInputData& data, const Print& print)
{
    assign_object_simulation(data, make_object_simulation_profile(print));
}

void clear_object_simulation(libvgcode::GCodeInputData& data)
{
    for (libvgcode::PathVertex& vertex : data.vertices) {
        vertex.object_simulation = 0.0f;
        vertex.object_simulation_reasons = libvgcode::ObjectSimulationNone;
        vertex.object_simulation_confidence = 0.0f;
        vertex.object_simulation_material_confidence = 0.0f;
        vertex.object_simulation_model_confidence = 0.0f;
    }
}

std::string object_simulation_reasons_to_string(uint32_t reasons)
{
    if (reasons == libvgcode::ObjectSimulationNone)
        return _u8L("N/A");

    std::vector<std::string> labels;
    auto add = [&labels, reasons](uint32_t flag, const std::string& label) {
        if ((reasons & flag) != 0)
            labels.emplace_back(label);
    };

    add(libvgcode::ObjectSimulationHighShrinkMaterial, _u8L("High-shrinkage material"));
    add(libvgcode::ObjectSimulationBottomLayer,       _u8L("Bottom layer"));
    add(libvgcode::ObjectSimulationOuterWall,         _u8L("Outer wall"));
    add(libvgcode::ObjectSimulationSharpCorner,       _u8L("Sharp corner"));
    add(libvgcode::ObjectSimulationLongPath,          _u8L("Long path"));
    add(libvgcode::ObjectSimulationLargeBottomArea,   _u8L("Large bottom area"));
    add(libvgcode::ObjectSimulationHighCooling,       _u8L("Excessive cooling"));
    add(libvgcode::ObjectSimulationLowChamber,        _u8L("Low chamber temperature"));
    add(libvgcode::ObjectSimulationHighThermalDelta,  _u8L("High thermal delta"));
    add(libvgcode::ObjectSimulationHighShrinkage,     _u8L("High shrinkage"));
    add(libvgcode::ObjectSimulationBrimRaftProtected, _u8L("Brim/raft protected"));
    add(libvgcode::ObjectSimulationHighThermalStress, _u8L("High thermal stress"));
    add(libvgcode::ObjectSimulationLowBed,            _u8L("Low bed temperature"));
    add(libvgcode::ObjectSimulationLowConfidence,     _u8L("Low data confidence"));

    std::string ret;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i > 0)
            ret += ", ";
        ret += labels[i];
    }
    return ret.empty() ? _u8L("N/A") : ret;
}

float preview_effective_object_simulation(const libvgcode::PathVertex& vertex)
{
    return vertex.object_simulation;
}

std::string object_simulation_level_label(float risk)
{
    if (risk < 0.34f)
        return _u8L("Low");
    if (risk < 0.67f)
        return _u8L("Medium");
    return _u8L("High");
}

std::string object_simulation_remaining_reason_label(uint32_t reasons)
{
    if ((reasons & libvgcode::ObjectSimulationLowChamber) != 0 || (reasons & libvgcode::ObjectSimulationLowBed) != 0 ||
        (reasons & libvgcode::ObjectSimulationHighThermalDelta) != 0)
        return _u8L("Remaining risk mainly comes from chamber, bed, or thermal delta");
    if ((reasons & libvgcode::ObjectSimulationHighShrinkMaterial) != 0 || (reasons & libvgcode::ObjectSimulationHighShrinkage) != 0)
        return _u8L("Remaining risk mainly comes from material shrinkage");
    if ((reasons & libvgcode::ObjectSimulationLargeBottomArea) != 0)
        return _u8L("Remaining risk mainly comes from a large bottom area");
    if ((reasons & libvgcode::ObjectSimulationSharpCorner) != 0)
        return _u8L("Remaining risk mainly comes from sharp-corner geometry");
    if ((reasons & libvgcode::ObjectSimulationLongPath) != 0)
        return _u8L("Remaining risk mainly comes from long paths");
    return _u8L("Remaining risk mainly comes from material, thermal control, or geometry");
}

namespace {

static std::string format_mm(float value)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(0) << value << " mm";
    return out.str();
}

static float get_float(const PrintConfig& config, const std::string& key, float fallback = 0.0f)
{
    const auto* option = config.option<ConfigOptionFloat>(key);
    return option != nullptr ? static_cast<float>(option->value) : fallback;
}

static int get_int(const PrintConfig& config, const std::string& key, int fallback = 0)
{
    const auto* option = config.option<ConfigOptionInt>(key);
    return option != nullptr ? option->value : fallback;
}

static float get_float_at(const PrintConfig& config, const std::string& key, size_t index, float fallback = 0.0f)
{
    const auto* option = config.option<ConfigOptionFloats>(key);
    return option != nullptr ? static_cast<float>(option->get_at(index)) : fallback;
}

static float get_percent_at(const PrintConfig& config, const std::string& key, size_t index, float fallback = 100.0f)
{
    const auto* option = config.option<ConfigOptionPercents>(key);
    return option != nullptr ? static_cast<float>(option->get_at(index)) : fallback;
}

static int get_int_at(const PrintConfig& config, const std::string& key, size_t index, int fallback = 0)
{
    const auto* option = config.option<ConfigOptionInts>(key);
    return option != nullptr ? option->get_at(index) : fallback;
}

static std::string get_string_at(const PrintConfig& config, const std::string& key, size_t index)
{
    const auto* option = config.option<ConfigOptionStrings>(key);
    return option != nullptr ? option->get_at(index) : std::string();
}

static void apply_material_defaults(const std::string& filament_type, ObjectSimulationAdvisorConfig& config)
{
    const std::string type = uppercase_ascii(filament_type);
    config.material_family = filament_type.empty() ? _u8L("Unknown") : filament_type;

    if (has_token(type, "PEEK") || has_token(type, "PEKK") || has_token(type, "PAEK") ||
        has_token(type, "PPSU") || has_token(type, "PPS") || has_token(type, "ULTEM") ||
        has_token(type, "PEI")) {
        config.material_family = _u8L("High-temperature polymer");
        config.material_sensitivity = 1.0f;
        config.recommended_chamber_temperature = 130.0f;
        config.recommended_bed_temperature = 130.0f;
        config.high_warp_material = true;
        config.material_profile_known = true;
        return;
    }

    if (has_token(type, "PC")) {
        config.material_family = "PC";
        config.material_sensitivity = 0.90f;
        config.recommended_chamber_temperature = 80.0f;
        config.recommended_bed_temperature = 110.0f;
        config.high_warp_material = true;
        config.material_profile_known = true;
        return;
    }

    if (has_token(type, "ABS") || has_token(type, "ASA")) {
        config.material_family = has_token(type, "ASA") ? "ASA" : "ABS";
        config.material_sensitivity = 0.85f;
        config.recommended_chamber_temperature = 60.0f;
        config.recommended_bed_temperature = 100.0f;
        config.high_warp_material = true;
        config.material_profile_known = true;
        return;
    }

    if (type == "PA" || has_token(type, "PA-") || has_token(type, "PA6") ||
        has_token(type, "PA12") || has_token(type, "PAHT") || has_token(type, "PPA")) {
        config.material_family = has_token(type, "PPA") ? "PPA" : "PA";
        config.material_sensitivity = 0.82f;
        config.recommended_chamber_temperature = 60.0f;
        config.recommended_bed_temperature = 90.0f;
        config.high_warp_material = true;
        config.material_profile_known = true;
        return;
    }

    if (has_token(type, "PET-CF") || has_token(type, "PET-GF") || has_token(type, "PETG-CF") ||
        has_token(type, "PETG-GF") || has_token(type, "PCTG")) {
        config.material_family = _u8L("Engineering composite");
        config.material_sensitivity = 0.62f;
        config.recommended_chamber_temperature = 45.0f;
        config.recommended_bed_temperature = 80.0f;
        config.material_profile_known = true;
        return;
    }

    if (has_token(type, "PETG") || has_token(type, "PET")) {
        config.material_family = has_token(type, "PETG") ? "PETG" : "PET";
        config.material_sensitivity = 0.35f;
        config.recommended_chamber_temperature = 35.0f;
        config.recommended_bed_temperature = 75.0f;
        config.material_profile_known = true;
        return;
    }

    if (has_token(type, "PLA")) {
        config.material_family = "PLA";
        config.material_sensitivity = 0.25f;
        config.recommended_chamber_temperature = 30.0f;
        config.recommended_bed_temperature = 55.0f;
        config.material_profile_known = true;
        return;
    }

    if (has_token(type, "TPU") || has_token(type, "PVA")) {
        config.material_family = has_token(type, "TPU") ? "TPU" : "PVA";
        config.material_sensitivity = 0.25f;
        config.recommended_chamber_temperature = 30.0f;
        config.recommended_bed_temperature = 55.0f;
        config.material_profile_known = true;
    }
}

static float safe_chamber_target(const ObjectSimulationAdvisorConfig& config)
{
    float target = config.recommended_chamber_temperature;
    if (config.temperature_vitrification > 10.0f)
        target = std::min(target, config.temperature_vitrification - 5.0f);
    if (config.material_sensitivity < 0.45f)
        target = std::min(target, 40.0f);
    return clampf(target, 0.0f, 160.0f);
}

static float safe_bed_target(const ObjectSimulationAdvisorConfig& config)
{
    float target = config.recommended_bed_temperature;
    if (config.material_sensitivity < 0.45f)
        target = std::min(target, 80.0f);
    return clampf(target, 0.0f, 160.0f);
}

struct ScanStats
{
    bool found{ false };
    libvgcode::PathVertex max_vertex;
    uint32_t reason_flags{ libvgcode::ObjectSimulationNone };
    std::array<int, 3> counts{ 0, 0, 0 };
    float confidence_sum{ 0.0f };
    float fan_sum{ 0.0f };
    float chamber_sum{ 0.0f };
    float bed_sum{ 0.0f };
    int samples{ 0 };
};

static ScanStats scan_vertices(const libvgcode::Viewer& viewer, size_t first, size_t last)
{
    ScanStats stats;
    if (first > last)
        std::swap(first, last);

    const size_t span = last - first + 1;
    const size_t stride = std::max<size_t>(1, span / MAX_SCAN_VERTICES + (span % MAX_SCAN_VERTICES != 0 ? 1 : 0));

    for (size_t i = first; i <= last; i += stride) {
        const libvgcode::PathVertex& vertex = viewer.get_vertex_at(i);
        if (!vertex.is_extrusion())
            continue;

        if (vertex.object_simulation < 0.34f)
            ++stats.counts[0];
        else if (vertex.object_simulation < 0.67f)
            ++stats.counts[1];
        else
            ++stats.counts[2];

        if (vertex.object_simulation >= 0.34f)
            stats.reason_flags |= vertex.object_simulation_reasons;

        stats.confidence_sum += vertex.object_simulation_confidence;
        stats.fan_sum += vertex.fan_speed;
        stats.chamber_sum += vertex.chamber_temperature;
        stats.bed_sum += vertex.bed_temperature;
        ++stats.samples;

        if (!stats.found || vertex.object_simulation > stats.max_vertex.object_simulation) {
            stats.max_vertex = vertex;
            stats.found = true;
        }

        if (last - i < stride)
            break;
    }

    if (stats.found)
        stats.reason_flags |= stats.max_vertex.object_simulation_reasons;

    return stats;
}

static bool has_reason(uint32_t reasons, uint32_t reason)
{
    return (reasons & reason) != 0;
}

static ObjectSimulationAdviceSeverity severity_for(float visible_risk, float expected_delta)
{
    if (visible_risk >= 0.67f || expected_delta >= 0.18f)
        return ObjectSimulationAdviceSeverity::High;
    if (visible_risk >= 0.45f || expected_delta >= 0.10f)
        return ObjectSimulationAdviceSeverity::Medium;
    return ObjectSimulationAdviceSeverity::Low;
}

static void add_change(ObjectSimulationAdvice& advice, ObjectSimulationConfigScope scope, const std::string& key,
                       ObjectSimulationConfigValueType type, double float_value, int int_value = 0)
{
    advice.target_config_keys.push_back(key);
    advice.changes.push_back({ scope, key, type, float_value, int_value });
}

static WarpThermalResolution to_warp_thermal_resolution(WarpPreventionThermalResolution resolution)
{
    switch (resolution) {
    case WarpPreventionThermalResolution::Fast: return WarpThermalResolution::Fast;
    case WarpPreventionThermalResolution::High: return WarpThermalResolution::High;
    case WarpPreventionThermalResolution::Auto:
    default: return WarpThermalResolution::Auto;
    }
}

static WarpPreventionLevel to_config_warp_prevention_level(WarpPreventionPresetLevel level)
{
    switch (level) {
    case WarpPreventionPresetLevel::Conservative: return WarpPreventionLevel::Conservative;
    case WarpPreventionPresetLevel::Aggressive:   return WarpPreventionLevel::Aggressive;
    case WarpPreventionPresetLevel::Balanced:
    default:                                      return WarpPreventionLevel::Balanced;
    }
}

static void add_warp_prevention_preset_changes(ObjectSimulationAdvice& advice, WarpPreventionPresetLevel level)
{
    const WarpPreventionPreset preset = warp_prevention_preset(level);
    add_change(advice, ObjectSimulationConfigScope::Print, "enable_warp_prevention", ObjectSimulationConfigValueType::Bool, 0.0, 1);
    add_change(advice, ObjectSimulationConfigScope::Print, "warp_prevention_level", ObjectSimulationConfigValueType::WarpPreventionLevel,
               0.0, static_cast<int>(to_config_warp_prevention_level(level)));
    add_change(advice, ObjectSimulationConfigScope::Print, "warp_prevention_max_slowdown", ObjectSimulationConfigValueType::Float,
               preset.max_slowdown_percent);
    add_change(advice, ObjectSimulationConfigScope::Print, "warp_prevention_min_wall_speed", ObjectSimulationConfigValueType::Float,
               preset.min_wall_speed);
    add_change(advice, ObjectSimulationConfigScope::Print, "warp_prevention_min_bottom_speed", ObjectSimulationConfigValueType::Float,
               preset.min_bottom_speed);
    add_change(advice, ObjectSimulationConfigScope::Print, "warp_prevention_early_layers", ObjectSimulationConfigValueType::Int,
               0.0, preset.max_active_layers);
    add_change(advice, ObjectSimulationConfigScope::Print, "warp_prevention_adjust_acceleration", ObjectSimulationConfigValueType::Bool,
               0.0, preset.adjust_acceleration ? 1 : 0);
    add_change(advice, ObjectSimulationConfigScope::Print, "warp_prevention_max_accel_reduction", ObjectSimulationConfigValueType::Float,
               preset.max_accel_reduction_percent);
    add_change(advice, ObjectSimulationConfigScope::Print, "warp_prevention_min_wall_acceleration", ObjectSimulationConfigValueType::Float,
               preset.min_wall_acceleration);
    add_change(advice, ObjectSimulationConfigScope::Print, "warp_prevention_min_bottom_acceleration", ObjectSimulationConfigValueType::Float,
               preset.min_bottom_acceleration);
}

static void add_advice(std::vector<ObjectSimulationAdvice>& out, std::set<std::string>& categories,
                       const std::string& key, ObjectSimulationAdvice advice)
{
    if (!advice.changes.empty() && categories.insert(key).second) {
        advice.can_apply = true;
        out.push_back(std::move(advice));
    }
}

static std::string join_display_parts(const std::vector<std::string>& parts)
{
    std::string out;
    for (const std::string& part : parts) {
        if (part.empty())
            continue;
        if (!out.empty())
            out += ", ";
        out += part;
    }
    return out;
}

static bool set_option_value(DynamicPrintConfig& config, const ObjectSimulationConfigChange& change)
{
    if (config.def() == nullptr || !config.def()->has(change.key))
        return false;

    const size_t vector_index = 0;

    switch (change.value_type) {
    case ObjectSimulationConfigValueType::Float:
        config.set_key_value(change.key, new ConfigOptionFloat(change.float_value));
        break;
    case ObjectSimulationConfigValueType::Int:
        config.set_key_value(change.key, new ConfigOptionInt(change.int_value));
        break;
    case ObjectSimulationConfigValueType::Bool:
        config.set_key_value(change.key, new ConfigOptionBool(change.int_value != 0));
        break;
    case ObjectSimulationConfigValueType::FloatVector: {
        auto* option = config.option<ConfigOptionFloats>(change.key);
        auto values = option != nullptr ? option->values : std::vector<double>{ change.float_value };
        if (values.empty())
            values.emplace_back(change.float_value);
        if (vector_index >= values.size())
            values.resize(vector_index + 1, values.front());
        values[vector_index] = change.float_value;
        config.set_key_value(change.key, new ConfigOptionFloats(std::move(values)));
        break;
    }
    case ObjectSimulationConfigValueType::IntVector: {
        auto* option = config.option<ConfigOptionInts>(change.key);
        auto values = option != nullptr ? option->values : std::vector<int>{ change.int_value };
        if (values.empty())
            values.emplace_back(change.int_value);
        if (vector_index >= values.size())
            values.resize(vector_index + 1, values.front());
        values[vector_index] = change.int_value;
        config.set_key_value(change.key, new ConfigOptionInts(std::move(values)));
        break;
    }
    case ObjectSimulationConfigValueType::BrimType:
        config.set_key_value(change.key, new ConfigOptionEnum<BrimType>(static_cast<BrimType>(change.int_value)));
        break;
    case ObjectSimulationConfigValueType::WarpPreventionLevel:
        config.set_key_value(change.key, new ConfigOptionEnum<WarpPreventionLevel>(static_cast<WarpPreventionLevel>(change.int_value)));
        break;
    case ObjectSimulationConfigValueType::WarpPreventionThermalResolution:
        config.set_key_value(change.key, new ConfigOptionEnum<WarpPreventionThermalResolution>(static_cast<WarpPreventionThermalResolution>(change.int_value)));
        break;
    }
    return true;
}

static bool config_value_changed(float lhs, float rhs)
{
    return std::fabs(lhs - rhs) > 0.01f;
}

static bool warp_prevention_settings_changed(const ObjectSimulationAdvisorConfig& sliced_config,
                                             const ObjectSimulationAdvisorConfig& current_config)
{
    return sliced_config.valid &&
        (sliced_config.warp_prevention_enabled != current_config.warp_prevention_enabled ||
         config_value_changed(sliced_config.warp_prevention_max_slowdown, current_config.warp_prevention_max_slowdown) ||
         config_value_changed(sliced_config.warp_prevention_min_wall_speed, current_config.warp_prevention_min_wall_speed) ||
         config_value_changed(sliced_config.warp_prevention_min_bottom_speed, current_config.warp_prevention_min_bottom_speed) ||
         sliced_config.warp_prevention_early_layers != current_config.warp_prevention_early_layers ||
         sliced_config.warp_prevention_adjust_acceleration != current_config.warp_prevention_adjust_acceleration ||
         config_value_changed(sliced_config.warp_prevention_max_accel_reduction, current_config.warp_prevention_max_accel_reduction) ||
         config_value_changed(sliced_config.warp_prevention_min_wall_acceleration, current_config.warp_prevention_min_wall_acceleration) ||
         config_value_changed(sliced_config.warp_prevention_min_bottom_acceleration, current_config.warp_prevention_min_bottom_acceleration));
}

} // namespace

ObjectSimulationAdvisorConfig make_object_simulation_advisor_config(const Print& print)
{
    ObjectSimulationAdvisorConfig config;
    const PrintConfig& print_config = print.config();
    const size_t extruder = 0;

    apply_material_defaults(get_string_at(print_config, "filament_type", extruder), config);
    config.valid = true;
    config.extruder_id = static_cast<int>(extruder);
    config.has_brim = print.has_brim();
    const auto objects = print.objects();
    config.has_raft = std::any_of(objects.begin(), objects.end(), [](const PrintObject* object) {
        return object != nullptr && object->has_raft();
    });

    config.temperature_vitrification = static_cast<float>(get_int_at(print_config, "temperature_vitrification", extruder, 0));
    config.chamber_temperature = static_cast<float>(get_int_at(print_config, "chamber_temperature", extruder, 0));
    config.hot_plate_temp = static_cast<float>(get_int_at(print_config, "hot_plate_temp", extruder, 0));
    config.hot_plate_temp_initial_layer = static_cast<float>(get_int_at(print_config, "hot_plate_temp_initial_layer", extruder, static_cast<int>(config.hot_plate_temp)));
    config.fan_min_speed = get_float_at(print_config, "fan_min_speed", extruder, 20.0f);
    config.fan_max_speed = get_float_at(print_config, "fan_max_speed", extruder, 100.0f);
    config.first_x_layer_fan_speed = get_float_at(print_config, "first_x_layer_fan_speed", extruder, 0.0f);
    config.additional_cooling_fan_speed = static_cast<float>(get_int_at(print_config, "additional_cooling_fan_speed", extruder, 0));
    config.outer_wall_speed = get_float(print_config, "outer_wall_speed", 0.0f);
    config.inner_wall_speed = get_float(print_config, "inner_wall_speed", 0.0f);
    config.initial_layer_speed = get_float(print_config, "initial_layer_speed", 0.0f);
    config.warp_prevention_enabled = print_config.enable_warp_prevention.value;
    config.warp_prevention_thermal_resolution = static_cast<int>(print_config.warp_prevention_thermal_resolution.value);
    config.warp_prevention_max_slowdown = static_cast<float>(print_config.warp_prevention_max_slowdown.value);
    config.warp_prevention_min_wall_speed = static_cast<float>(print_config.warp_prevention_min_wall_speed.value);
    config.warp_prevention_min_bottom_speed = static_cast<float>(print_config.warp_prevention_min_bottom_speed.value);
    config.warp_prevention_early_layers = std::max(0, print_config.warp_prevention_early_layers.value);
    config.warp_prevention_adjust_acceleration = print_config.warp_prevention_adjust_acceleration.value;
    config.warp_prevention_max_accel_reduction = static_cast<float>(print_config.warp_prevention_max_accel_reduction.value);
    config.warp_prevention_min_wall_acceleration = static_cast<float>(print_config.warp_prevention_min_wall_acceleration.value);
    config.warp_prevention_min_bottom_acceleration = static_cast<float>(print_config.warp_prevention_min_bottom_acceleration.value);
    config.brim_width = get_float(print_config, "brim_width", 0.0f);
    config.raft_layers = get_int(print_config, "raft_layers", 0);
    config.brim_ears_max_angle = get_float(print_config, "brim_ears_max_angle", 125.0f);
    config.brim_ears_detection_length = get_float(print_config, "brim_ears_detection_length", 1.0f);

    if (const auto* brim = print_config.option<ConfigOptionEnum<BrimType>>("brim_type"); brim != nullptr)
        config.brim_type = static_cast<int>(brim->value);

    const float shrink = get_percent_at(print_config, "filament_shrink", extruder, 100.0f);
    if (std::fabs(shrink - 100.0f) > 0.5f) {
        config.material_sensitivity = std::max(config.material_sensitivity, 0.70f);
        config.high_warp_material = true;
        config.material_profile_known = true;
    }

    return config;
}

ObjectSimulationRuntimeConfigState make_object_simulation_runtime_config_state(
    const ObjectSimulationAdvisorConfig& sliced_config,
    const DynamicPrintConfig* current_print_config)
{
    ObjectSimulationRuntimeConfigState state;
    state.config = sliced_config;
    state.sliced_config_valid = sliced_config.valid;
    state.sliced_warp_prevention_enabled = sliced_config.valid && sliced_config.warp_prevention_enabled;
    state.current_warp_prevention_enabled = sliced_config.valid && sliced_config.warp_prevention_enabled;

    ObjectSimulationAdvisorConfig& config = state.config;
    if (current_print_config != nullptr) {
        config.valid = true;
        if (const auto* opt = current_print_config->option<ConfigOptionBool>("enable_warp_prevention"); opt != nullptr)
            state.current_warp_prevention_enabled = opt->value;
        if (const auto* opt = current_print_config->option<ConfigOptionEnum<WarpPreventionThermalResolution>>("warp_prevention_thermal_resolution"); opt != nullptr)
            config.warp_prevention_thermal_resolution = static_cast<int>(opt->value);
        if (const auto* opt = current_print_config->option<ConfigOptionFloat>("warp_prevention_max_slowdown"); opt != nullptr)
            config.warp_prevention_max_slowdown = static_cast<float>(opt->value);
        if (const auto* opt = current_print_config->option<ConfigOptionFloat>("warp_prevention_min_wall_speed"); opt != nullptr)
            config.warp_prevention_min_wall_speed = static_cast<float>(opt->value);
        if (const auto* opt = current_print_config->option<ConfigOptionFloat>("warp_prevention_min_bottom_speed"); opt != nullptr)
            config.warp_prevention_min_bottom_speed = static_cast<float>(opt->value);
        if (const auto* opt = current_print_config->option<ConfigOptionInt>("warp_prevention_early_layers"); opt != nullptr)
            config.warp_prevention_early_layers = std::max(0, opt->value);
        if (const auto* opt = current_print_config->option<ConfigOptionBool>("warp_prevention_adjust_acceleration"); opt != nullptr)
            config.warp_prevention_adjust_acceleration = opt->value;
        if (const auto* opt = current_print_config->option<ConfigOptionFloat>("warp_prevention_max_accel_reduction"); opt != nullptr)
            config.warp_prevention_max_accel_reduction = static_cast<float>(opt->value);
        if (const auto* opt = current_print_config->option<ConfigOptionFloat>("warp_prevention_min_wall_acceleration"); opt != nullptr)
            config.warp_prevention_min_wall_acceleration = static_cast<float>(opt->value);
        if (const auto* opt = current_print_config->option<ConfigOptionFloat>("warp_prevention_min_bottom_acceleration"); opt != nullptr)
            config.warp_prevention_min_bottom_acceleration = static_cast<float>(opt->value);
        if (const auto* opt = current_print_config->option<ConfigOptionEnum<BrimType>>("brim_type"); opt != nullptr)
            config.brim_type = static_cast<int>(opt->value);
        if (const auto* opt = current_print_config->option<ConfigOptionFloat>("brim_width"); opt != nullptr) {
            config.brim_width = static_cast<float>(opt->value);
            config.has_brim = config.brim_type != static_cast<int>(btNoBrim) && config.brim_width > 0.0f;
        }
        if (const auto* opt = current_print_config->option<ConfigOptionFloat>("outer_wall_speed"); opt != nullptr)
            config.outer_wall_speed = static_cast<float>(opt->value);
        if (const auto* opt = current_print_config->option<ConfigOptionFloat>("inner_wall_speed"); opt != nullptr)
            config.inner_wall_speed = static_cast<float>(opt->value);
        if (const auto* opt = current_print_config->option<ConfigOptionFloat>("initial_layer_speed"); opt != nullptr)
            config.initial_layer_speed = static_cast<float>(opt->value);
    }

    config.warp_prevention_enabled = state.current_warp_prevention_enabled;
    state.pending_reslice = state.current_warp_prevention_enabled &&
        (!state.sliced_config_valid || !state.sliced_warp_prevention_enabled || warp_prevention_settings_changed(sliced_config, config));
    state.warp_layers_disabled = state.current_warp_prevention_enabled && config.warp_prevention_early_layers <= 0;
    return state;
}

ObjectSimulationPreviewSummary make_object_simulation_preview_summary(const libvgcode::Viewer& viewer)
{
    ObjectSimulationPreviewSummary summary;
    const size_t vertices_count = viewer.get_vertices_count();
    if (vertices_count == 0)
        return summary;

    float max_display_risk = 0.0f;
    for (size_t i = 0; i < vertices_count; ++i) {
        const libvgcode::PathVertex& vertex = viewer.get_vertex_at(i);
        if (!vertex.is_extrusion())
            continue;

        ++summary.extrusion_count;
        const float display_risk = vertex.object_simulation;
        if (vertex.object_simulation_material_confidence > 0.0f || vertex.object_simulation_model_confidence > 0.0f) {
            summary.material_confidence_sum += vertex.object_simulation_material_confidence;
            summary.simulation_confidence_sum += vertex.object_simulation_model_confidence;
            ++summary.confidence_count;
        }

        if (display_risk < 0.34f)
            ++summary.low_count;
        else if (display_risk < 0.67f)
            ++summary.medium_count;
        else
            ++summary.high_count;
        if (display_risk >= 0.34f)
            summary.reason_flags |= vertex.object_simulation_reasons;

        if (!summary.found || display_risk > max_display_risk) {
            max_display_risk = display_risk;
            summary.max_vertex_index = i;
            summary.found = true;
        }
    }
    if (summary.found)
        summary.reason_flags |= viewer.get_vertex_at(summary.max_vertex_index).object_simulation_reasons;

    return summary;
}

ObjectSimulationPanelText make_object_simulation_panel_text(
    const libvgcode::PathVertex& max_vertex,
    const ObjectSimulationPreviewSummary& preview_summary,
    const ObjectSimulationRuntimeConfigState& runtime_config)
{
    ObjectSimulationPanelText text;
    const ObjectSimulationAdvisorConfig& config = runtime_config.config;
    const float current_max_risk = preview_effective_object_simulation(max_vertex);

    {
        std::ostringstream out;
        out << std::fixed << std::setprecision(0) << current_max_risk * 100.0f << " % (" << object_simulation_level_label(current_max_risk) << ")";
        text.current_risk = out.str();
    }

    if (runtime_config.pending_reslice)
        text.next_step = _u8L("Settings changed. Use the slice button above to regenerate G-code.");
    else if (runtime_config.warp_layers_disabled)
        text.next_step = _u8L("Max active layers is 0, so warp-prevention speed and fan changes are disabled. Increase it and reslice to emit warp-prevention G-code.");
    else if (!runtime_config.current_warp_prevention_enabled && current_max_risk >= object_simulation_advice_threshold)
        text.next_step = _u8L("Warp prevention is off. Enable it and reslice to write smooth speed, fan, and acceleration changes into G-code.");
    else if (current_max_risk < 0.34f)
        text.next_step = _u8L("Current simulation risk is low. Keep the current settings.");
    else if (runtime_config.current_warp_prevention_enabled)
        text.next_step = _u8L("Warp prevention is enabled. Remaining risk comes from material, thermal control, or geometry.");
    else
        text.next_step = _u8L("Review material, chamber, bed, and geometry settings.");

    if (!runtime_config.current_warp_prevention_enabled)
        text.processing_status = _u8L("Off; this G-code has no warp-prevention process changes.");
    else if (runtime_config.warp_layers_disabled)
        text.processing_status = _u8L("Analysis only; max active layers is 0.");
    else if (runtime_config.pending_reslice)
        text.processing_status = _u8L("Settings changed; reslice is required.");
    else
        text.processing_status = _u8L("Enabled; exported G-code stays clean and uses standard speed, fan, and acceleration commands.");

    text.added_time = _u8L("Included in the G-code estimate");
    const uint32_t global_reasons = preview_summary.reason_flags | max_vertex.object_simulation_reasons;
    text.remaining_risk = current_max_risk >= 0.45f ?
        object_simulation_remaining_reason_label(global_reasons) :
        _u8L("Low enough for the current settings.");

    if (runtime_config.current_warp_prevention_enabled && current_max_risk >= 0.45f) {
        if (config.warp_prevention_max_slowdown < 60.0f)
            text.useful_adjustments.emplace_back(_u8L("Increase maximum slowdown"));

        const bool bottom_like_role = max_vertex.role == libvgcode::EGCodeExtrusionRole::BottomSurface ||
            max_vertex.role == libvgcode::EGCodeExtrusionRole::SolidInfill ||
            max_vertex.role == libvgcode::EGCodeExtrusionRole::InternalInfill;
        if (bottom_like_role) {
            if (config.warp_prevention_min_bottom_speed > 8.0f)
                text.useful_adjustments.emplace_back(_u8L("Lower minimum bottom speed"));
        } else if (config.warp_prevention_min_wall_speed > 8.0f) {
            text.useful_adjustments.emplace_back(_u8L("Lower minimum wall speed"));
        }

        if (config.warp_prevention_early_layers <= 0 ||
            (max_vertex.layer_id + 1 >= static_cast<uint32_t>(config.warp_prevention_early_layers) &&
             config.warp_prevention_early_layers < 999))
            text.useful_adjustments.emplace_back(_u8L("Increase max active layers"));

        if (text.useful_adjustments.empty())
            text.useful_adjustments.emplace_back(_u8L("Raise chamber/bed temperature or add fillets"));
    }

    return text;
}

std::vector<ObjectSimulationTextRow> make_object_simulation_detail_rows(
    const libvgcode::PathVertex& max_vertex,
    const ObjectSimulationPreviewSummary& preview_summary,
    const ObjectSimulationRuntimeConfigState& runtime_config,
    const ObjectSimulationPanelText& panel_text)
{
    std::vector<ObjectSimulationTextRow> rows;
    const ObjectSimulationAdvisorConfig& config = runtime_config.config;

    rows.push_back({ _u8L("Highest display risk"), format_percent(max_vertex.object_simulation * 100.0f) });
    rows.push_back({ _u8L("Layer"), std::to_string(max_vertex.layer_id + 1) });
    rows.push_back({ _u8L("Role"), object_simulation_role_label(max_vertex.role) });
    rows.push_back({ _u8L("Reasons"), object_simulation_reasons_to_string(max_vertex.object_simulation_reasons) });
    rows.push_back({ _u8L("Warp prevention"), runtime_config.current_warp_prevention_enabled ? _u8L("Enabled in current settings") : _u8L("Disabled") });
    if (!runtime_config.current_warp_prevention_enabled)
        rows.push_back({ _u8L("G-code status"), _u8L("Warp prevention was off for this slice, so speed, fan, and acceleration were not changed.") });
    rows.push_back({ _u8L("Thermal model resolution"), object_simulation_thermal_resolution_label(config.warp_prevention_thermal_resolution) });
    if (runtime_config.current_warp_prevention_enabled) {
        std::ostringstream out;
        out << config.warp_prevention_early_layers << " " << _u8L("layers")
            << ", " << std::fixed << std::setprecision(0) << config.warp_prevention_max_slowdown
            << " %, " << config.warp_prevention_min_wall_speed << "/"
            << config.warp_prevention_min_bottom_speed << " mm/s";
        rows.push_back({ _u8L("Warp prevention parameters"), out.str() });
    }
    if (runtime_config.current_warp_prevention_enabled &&
        (!runtime_config.sliced_config_valid || !runtime_config.sliced_warp_prevention_enabled))
        rows.push_back({ _u8L("G-code status"), _u8L("Not resliced yet") });
    if (preview_summary.confidence_count > 0) {
        std::ostringstream out;
        out << _u8L("Material") << " " << std::fixed << std::setprecision(0)
            << preview_summary.material_confidence_sum * 100.0f / static_cast<float>(preview_summary.confidence_count)
            << "%, " << _u8L("Local model") << " "
            << preview_summary.simulation_confidence_sum * 100.0f / static_cast<float>(preview_summary.confidence_count) << "%";
        rows.push_back({ _u8L("Material / simulation confidence"), out.str() });
    }
    if (!panel_text.useful_adjustments.empty())
        rows.push_back({ _u8L("Useful adjustments"), join_display_parts(panel_text.useful_adjustments) });

    return rows;
}

std::vector<ObjectSimulationAdviceView> make_object_simulation_advice_view(
    const ObjectSimulationOptimizationSummary& optimization,
    const DynamicPrintConfig* print_config,
    const DynamicPrintConfig* filament_config)
{
    std::vector<ObjectSimulationAdviceView> view;
    if (!optimization.valid)
        return view;

    view.reserve(optimization.advice.size());
    for (const ObjectSimulationAdvice& advice : optimization.advice) {
        const bool would_change = advice.can_apply && print_config != nullptr && filament_config != nullptr &&
            object_simulation_advice_changes_config(advice, *print_config, *filament_config);
        view.push_back({ advice, would_change });
    }
    return view;
}

ObjectSimulationOptimizationSummary make_object_simulation_optimization_summary(const libvgcode::Viewer& viewer, const ObjectSimulationAdvisorConfig& config)
{
    ObjectSimulationOptimizationSummary summary;
    const size_t vertices_count = viewer.get_vertices_count();
    if (vertices_count == 0)
        return summary;

    const ScanStats model = scan_vertices(viewer, 0, vertices_count - 1);
    if (!model.found)
        return summary;

    const libvgcode::PathVertex& max_vertex = model.max_vertex;
    const float visible_risk = max_vertex.object_simulation;
    const uint32_t reasons = model.reason_flags | max_vertex.object_simulation_reasons;
    const float avg_confidence = model.samples > 0 ? model.confidence_sum / static_cast<float>(model.samples) : max_vertex.object_simulation_confidence;

    summary.valid = true;
    summary.visible_highest_risk = visible_risk;
    summary.model_highest_risk = model.max_vertex.object_simulation;
    summary.visible_reason_flags = reasons;
    summary.model_reason_flags = model.reason_flags;
    summary.visible_risk_counts = model.counts;

    std::set<std::string> categories;
    constexpr float target_low_risk = 0.34f;

    if (!config.warp_prevention_enabled && visible_risk >= object_simulation_advice_threshold) {
        const WarpPreventionPresetLevel candidate_level = WarpPreventionPresetLevel::Balanced;
        const float simulated_speed_delta = std::min(visible_risk - target_low_risk, 0.20f);
        ObjectSimulationAdvice advice;
        advice.severity = severity_for(visible_risk, std::max(0.0f, simulated_speed_delta));
        advice.category = ObjectSimulationAdviceCategory::Speed;
        advice.reason_flags = reasons;
        advice.current_value = _u8L("Disabled");
        advice.recommended_value = _u8L("Enable Balanced warp prevention");
        advice.message = _u8L("Enable warp prevention");
        advice.detail = _u8L("This does not change the original speed presets. After reslicing, local G-code slowdown is emitted only on high-risk early-layer paths.");
        advice.expected_risk_delta = std::max(0.0f, simulated_speed_delta);
        advice.confidence = avg_confidence;
        add_warp_prevention_preset_changes(advice, candidate_level);
        add_advice(summary.advice, categories, "warp_prevention", std::move(advice));
    }

    if (has_reason(reasons, libvgcode::ObjectSimulationHighCooling) ||
        max_vertex.fan_speed > (config.high_warp_material ? 20.0f : 55.0f)) {
        const float fan_target = config.high_warp_material ? 15.0f : (config.material_sensitivity >= 0.60f ? 30.0f : 55.0f);
        if (config.fan_max_speed > fan_target + 1.0f || config.first_x_layer_fan_speed > 1.0f || config.additional_cooling_fan_speed > 1.0f) {
            ObjectSimulationAdvice advice;
            advice.severity = severity_for(visible_risk, 0.16f);
            advice.category = ObjectSimulationAdviceCategory::Cooling;
            advice.reason_flags = libvgcode::ObjectSimulationHighCooling;
            advice.current_value = _u8L("fan") + " " + format_percent(config.fan_max_speed) + ", " + _u8L("first layer") + " " + format_percent(config.first_x_layer_fan_speed);
            advice.recommended_value = _u8L("fan") + " <= " + format_percent(fan_target) + ", " + _u8L("first layer") + " 0 %";
            advice.message = _u8L("Reduce cooling fan");
            advice.detail = _u8L("High cooling increases thermal gradient on warp-prone paths; bridge and support cooling should stay controlled by their own settings.");
            advice.expected_risk_delta = 0.16f;
            advice.confidence = avg_confidence;
            add_change(advice, ObjectSimulationConfigScope::Filament, "first_x_layer_fan_speed", ObjectSimulationConfigValueType::FloatVector, 0.0);
            add_change(advice, ObjectSimulationConfigScope::Filament, "fan_min_speed", ObjectSimulationConfigValueType::FloatVector, std::min(config.fan_min_speed, fan_target));
            add_change(advice, ObjectSimulationConfigScope::Filament, "fan_max_speed", ObjectSimulationConfigValueType::FloatVector, fan_target);
            add_change(advice, ObjectSimulationConfigScope::Filament, "additional_cooling_fan_speed", ObjectSimulationConfigValueType::IntVector, 0.0, 0);
            add_advice(summary.advice, categories, "cooling", std::move(advice));
        }
    }

    if (has_reason(reasons, libvgcode::ObjectSimulationLowChamber) || has_reason(reasons, libvgcode::ObjectSimulationHighThermalDelta)) {
        const float chamber_target = safe_chamber_target(config);
        if (chamber_target > config.chamber_temperature + 2.0f) {
            ObjectSimulationAdvice advice;
            advice.severity = severity_for(visible_risk, 0.20f);
            advice.category = ObjectSimulationAdviceCategory::Chamber;
            advice.reason_flags = libvgcode::ObjectSimulationLowChamber | libvgcode::ObjectSimulationHighThermalDelta;
            advice.current_value = format_temperature(config.chamber_temperature);
            advice.recommended_value = format_temperature(chamber_target);
            advice.message = _u8L("Raise chamber temperature");
            advice.detail = _u8L("Higher chamber temperature lowers the cooling gradient before the material locks in shrink stress.");
            advice.expected_risk_delta = 0.20f;
            advice.confidence = avg_confidence;
            add_change(advice, ObjectSimulationConfigScope::Filament, "chamber_temperature", ObjectSimulationConfigValueType::IntVector, chamber_target, static_cast<int>(std::lround(chamber_target)));
            add_advice(summary.advice, categories, "chamber", std::move(advice));
        }
    }

    if (has_reason(reasons, libvgcode::ObjectSimulationLowBed) || has_reason(reasons, libvgcode::ObjectSimulationBottomLayer) ||
        has_reason(reasons, libvgcode::ObjectSimulationLargeBottomArea)) {
        const float bed_target = safe_bed_target(config);
        const float current_bed = std::max(config.hot_plate_temp, config.hot_plate_temp_initial_layer);
        if (bed_target > current_bed + 2.0f) {
            ObjectSimulationAdvice advice;
            advice.severity = severity_for(visible_risk, 0.14f);
            advice.category = ObjectSimulationAdviceCategory::Bed;
            advice.reason_flags = libvgcode::ObjectSimulationLowBed | libvgcode::ObjectSimulationBottomLayer | libvgcode::ObjectSimulationLargeBottomArea;
            advice.current_value = _u8L("bed") + " " + format_temperature(config.hot_plate_temp) + ", " + _u8L("first layer") + " " + format_temperature(config.hot_plate_temp_initial_layer);
            advice.recommended_value = _u8L("bed") + " " + format_temperature(bed_target) + ", " + _u8L("first layer") + " " + format_temperature(bed_target);
            advice.message = _u8L("Raise bed temperature");
            advice.detail = _u8L("A warmer bed reduces the early-layer thermal drop and improves adhesion while the bottom layers shrink.");
            advice.expected_risk_delta = 0.14f;
            advice.confidence = avg_confidence;
            add_change(advice, ObjectSimulationConfigScope::Filament, "hot_plate_temp_initial_layer", ObjectSimulationConfigValueType::IntVector, bed_target, static_cast<int>(std::lround(bed_target)));
            add_change(advice, ObjectSimulationConfigScope::Filament, "hot_plate_temp", ObjectSimulationConfigValueType::IntVector, bed_target, static_cast<int>(std::lround(bed_target)));
            add_advice(summary.advice, categories, "bed", std::move(advice));
        }
    }

    if ((has_reason(reasons, libvgcode::ObjectSimulationBottomLayer) || has_reason(reasons, libvgcode::ObjectSimulationLargeBottomArea)) &&
        (!config.has_brim || config.brim_width < (config.high_warp_material ? 10.0f : 6.0f))) {
        const float target_width = visible_risk >= 0.67f || config.high_warp_material ? 12.0f : 8.0f;
        ObjectSimulationAdvice advice;
        advice.severity = severity_for(visible_risk, 0.18f);
        advice.category = ObjectSimulationAdviceCategory::Adhesion;
        advice.reason_flags = libvgcode::ObjectSimulationBottomLayer | libvgcode::ObjectSimulationLargeBottomArea;
        advice.current_value = _u8L("Brim width") + " " + format_mm(config.brim_width);
        advice.recommended_value = _u8L("Inner and outer brim") + ", " + _u8L("Brim width") + " " + format_mm(target_width);
        advice.message = _u8L("Increase brim adhesion");
        advice.detail = _u8L("Large bottom areas and first-layer edges need inner and outer bed contact. This changes the toolpath after reslicing.");
        advice.expected_risk_delta = 0.18f;
        advice.confidence = avg_confidence;
        add_change(advice, ObjectSimulationConfigScope::Print, "brim_type", ObjectSimulationConfigValueType::BrimType, 0.0, static_cast<int>(btOuterAndInner));
        add_change(advice, ObjectSimulationConfigScope::Print, "brim_width", ObjectSimulationConfigValueType::Float, target_width);
        add_advice(summary.advice, categories, "brim", std::move(advice));
    }

    if (has_reason(reasons, libvgcode::ObjectSimulationSharpCorner) &&
        (config.brim_type != static_cast<int>(btEar) || config.brim_width < 8.0f)) {
        ObjectSimulationAdvice advice;
        advice.severity = severity_for(visible_risk, 0.15f);
        advice.category = ObjectSimulationAdviceCategory::Geometry;
        advice.reason_flags = libvgcode::ObjectSimulationSharpCorner;
        advice.current_value = _u8L("Sharp corners") + ", " + _u8L("Brim width") + " " + format_mm(config.brim_width);
        advice.recommended_value = _u8L("Mouse-ear brim") + ", " + _u8L("Brim width") + " 8 mm";
        advice.message = _u8L("Add mouse-ear adhesion");
        advice.detail = _u8L("Mouse ears add local anchoring area near sharp corners without forcing a wide brim around the entire model.");
        advice.expected_risk_delta = 0.15f;
        advice.confidence = avg_confidence;
        add_change(advice, ObjectSimulationConfigScope::Print, "brim_type", ObjectSimulationConfigValueType::BrimType, 0.0, static_cast<int>(btEar));
        add_change(advice, ObjectSimulationConfigScope::Print, "brim_width", ObjectSimulationConfigValueType::Float, std::max(config.brim_width, 8.0f));
        add_change(advice, ObjectSimulationConfigScope::Print, "brim_ears_max_angle", ObjectSimulationConfigValueType::Float, 125.0);
        add_change(advice, ObjectSimulationConfigScope::Print, "brim_ears_detection_length", ObjectSimulationConfigValueType::Float, 1.0);
        add_advice(summary.advice, categories, "ears", std::move(advice));
    }

    if (visible_risk >= 0.85f && config.high_warp_material && config.raft_layers == 0 && !config.has_raft) {
        ObjectSimulationAdvice advice;
        advice.severity = ObjectSimulationAdviceSeverity::High;
        advice.category = ObjectSimulationAdviceCategory::Adhesion;
        advice.reason_flags = libvgcode::ObjectSimulationBottomLayer | libvgcode::ObjectSimulationLargeBottomArea;
        advice.current_value = _u8L("raft layers") + " 0";
        advice.recommended_value = _u8L("raft layers") + " 2";
        advice.message = _u8L("Use raft for severe bottom warp");
        advice.detail = _u8L("Raft is reserved for severe high-temperature material cases because it changes the bottom surface and print time.");
        advice.expected_risk_delta = 0.22f;
        advice.confidence = avg_confidence;
        add_change(advice, ObjectSimulationConfigScope::Print, "raft_layers", ObjectSimulationConfigValueType::Int, 0.0, 2);
        add_advice(summary.advice, categories, "raft", std::move(advice));
    }

    if (!config.warp_prevention_enabled &&
        (has_reason(reasons, libvgcode::ObjectSimulationLongPath) || has_reason(reasons, libvgcode::ObjectSimulationOuterWall))) {
        const float outer_target = config.outer_wall_speed > 0.0f ? std::max(20.0f, config.outer_wall_speed * 0.80f) : 0.0f;
        const float inner_target = config.inner_wall_speed > 0.0f ? std::max(25.0f, config.inner_wall_speed * 0.85f) : 0.0f;
        const float initial_target = config.initial_layer_speed > 0.0f ? std::max(15.0f, config.initial_layer_speed * 0.80f) : 0.0f;
        const bool change_outer = outer_target > 0.0f && outer_target < config.outer_wall_speed - 0.5f;
        const bool change_inner = inner_target > 0.0f && inner_target < config.inner_wall_speed - 0.5f;
        const bool change_initial = initial_target > 0.0f && initial_target < config.initial_layer_speed - 0.5f;
        if (change_outer || change_inner || change_initial) {
            ObjectSimulationAdvice advice;
            advice.severity = severity_for(visible_risk, 0.10f);
            advice.category = ObjectSimulationAdviceCategory::Speed;
            advice.reason_flags = libvgcode::ObjectSimulationLongPath | libvgcode::ObjectSimulationOuterWall;
            std::vector<std::string> current_parts;
            std::vector<std::string> recommended_parts;
            if (change_outer) {
        current_parts.push_back(_u8L("Outer wall") + " " + format_speed(config.outer_wall_speed));
        recommended_parts.push_back(_u8L("Outer wall") + " " + format_speed(outer_target));
                add_change(advice, ObjectSimulationConfigScope::Print, "outer_wall_speed", ObjectSimulationConfigValueType::Float, outer_target);
            }
            if (change_inner) {
        current_parts.push_back(_u8L("Inner wall") + " " + format_speed(config.inner_wall_speed));
        recommended_parts.push_back(_u8L("Inner wall") + " " + format_speed(inner_target));
                add_change(advice, ObjectSimulationConfigScope::Print, "inner_wall_speed", ObjectSimulationConfigValueType::Float, inner_target);
            }
            if (change_initial) {
        current_parts.push_back(_u8L("Initial layer") + " " + format_speed(config.initial_layer_speed));
        recommended_parts.push_back(_u8L("Initial layer") + " " + format_speed(initial_target));
                add_change(advice, ObjectSimulationConfigScope::Print, "initial_layer_speed", ObjectSimulationConfigValueType::Float, initial_target);
            }
            advice.current_value = join_display_parts(current_parts);
            advice.recommended_value = join_display_parts(recommended_parts);
        advice.message = _u8L("Reduce high-risk wall speed");
        advice.detail = _u8L("Only speed items that would actually change are shown. Slower long wall paths add more thermal equalization time and lower locked-in stress.");
            advice.expected_risk_delta = 0.10f;
            advice.confidence = avg_confidence;
            add_advice(summary.advice, categories, "speed", std::move(advice));
        }
    }

    if (has_reason(reasons, libvgcode::ObjectSimulationHighThermalDelta) && !categories.count("chamber") && !categories.count("bed")) {
        ObjectSimulationAdvice advice;
        advice.severity = severity_for(visible_risk, 0.08f);
        advice.category = ObjectSimulationAdviceCategory::ThermalModel;
        advice.reason_flags = libvgcode::ObjectSimulationHighThermalDelta;
        advice.current_value = _u8L("thermal delta") + " " + format_temperature(std::max(0.0f, max_vertex.temperature - std::max(max_vertex.bed_temperature, max_vertex.chamber_temperature)));
        advice.recommended_value = _u8L("raise chamber/bed first; lower nozzle only within material range");
        advice.message = _u8L("Reduce thermal delta");
        advice.detail = _u8L("The fastest safe change is usually chamber or bed heat; nozzle temperature changes need material-specific validation.");
        advice.expected_risk_delta = 0.08f;
        advice.confidence = avg_confidence;
        add_advice(summary.advice, categories, "thermal_delta", std::move(advice));
    }

    if (has_reason(reasons, libvgcode::ObjectSimulationLowConfidence) || !config.material_profile_known || avg_confidence < 0.65f) {
        ObjectSimulationAdvice advice;
        advice.severity = ObjectSimulationAdviceSeverity::Medium;
        advice.category = ObjectSimulationAdviceCategory::Material;
        advice.reason_flags = libvgcode::ObjectSimulationLowConfidence;
        advice.current_value = config.material_profile_known ? _u8L("partial material data") : _u8L("unknown material family");
        advice.recommended_value = object_simulation_config_keys_label({ "filament_shrink", "temperature_vitrification", "filament_type" });
        advice.message = _u8L("Complete material properties");
        advice.detail = _u8L("Material shrink, vitrification temperature, and material family improve the thermal-stress estimate and advice confidence.");
        advice.expected_risk_delta = 0.0f;
        advice.confidence = std::min(avg_confidence, 0.55f);
        advice.target_config_keys = { "filament_shrink", "temperature_vitrification", "filament_type" };
        add_advice(summary.advice, categories, "material", std::move(advice));
    }

    if (visible_risk >= 0.67f && config.warp_prevention_thermal_resolution != static_cast<int>(WarpPreventionThermalResolution::High)) {
        ObjectSimulationAdvice advice;
        advice.severity = ObjectSimulationAdviceSeverity::Medium;
        advice.category = ObjectSimulationAdviceCategory::ThermalModel;
        advice.reason_flags = reasons;
        advice.current_value = object_simulation_thermal_resolution_label(config.warp_prevention_thermal_resolution);
        advice.recommended_value = object_simulation_thermal_resolution_label(static_cast<int>(WarpPreventionThermalResolution::High));
        advice.message = _u8L("Use high thermal model resolution");
        advice.detail = _u8L("High resolution uses a finer local thermal-history grid for high-temperature materials. It improves advice quality but may increase slicing time.");
        advice.requires_reslice = true;
        advice.expected_risk_delta = 0.0f;
        advice.confidence = avg_confidence;
        add_change(advice, ObjectSimulationConfigScope::Print, "warp_prevention_thermal_resolution",
                   ObjectSimulationConfigValueType::WarpPreventionThermalResolution,
                   0.0, static_cast<int>(WarpPreventionThermalResolution::High));
        add_advice(summary.advice, categories, "thermal_resolution", std::move(advice));
    }

    std::sort(summary.advice.begin(), summary.advice.end(), [](const ObjectSimulationAdvice& a, const ObjectSimulationAdvice& b) {
        if (a.severity != b.severity)
            return static_cast<int>(a.severity) > static_cast<int>(b.severity);
        if (a.expected_risk_delta != b.expected_risk_delta)
            return a.expected_risk_delta > b.expected_risk_delta;
        return a.message < b.message;
    });

    return summary;
}

bool apply_object_simulation_advice(const ObjectSimulationAdvice& advice, DynamicPrintConfig& print_config, DynamicPrintConfig& filament_config)
{
    if (!advice.can_apply)
        return false;

    bool modified = false;
    for (const ObjectSimulationConfigChange& change : advice.changes) {
        if (change.scope == ObjectSimulationConfigScope::Print)
            modified = set_option_value(print_config, change) || modified;
        else if (change.scope == ObjectSimulationConfigScope::Filament)
            modified = set_option_value(filament_config, change) || modified;
    }
    return modified;
}

bool object_simulation_advice_changes_config(
    const ObjectSimulationAdvice& advice,
    const DynamicPrintConfig& print_config,
    const DynamicPrintConfig& filament_config)
{
    DynamicPrintConfig next_print_config;
    DynamicPrintConfig next_filament_config;
    bool print_changed = false;
    bool filament_changed = false;
    if (!prepare_object_simulation_advice_config_change(advice, print_config, filament_config,
            next_print_config, next_filament_config, print_changed, filament_changed))
        return false;

    return print_changed || filament_changed;
}

bool prepare_object_simulation_advice_config_change(
    const ObjectSimulationAdvice& advice,
    const DynamicPrintConfig& print_config,
    const DynamicPrintConfig& filament_config,
    DynamicPrintConfig& next_print_config,
    DynamicPrintConfig& next_filament_config,
    bool& print_changed,
    bool& filament_changed)
{
    next_print_config = print_config;
    next_filament_config = filament_config;
    print_changed = false;
    filament_changed = false;

    if (!apply_object_simulation_advice(advice, next_print_config, next_filament_config))
        return false;

    print_changed = !print_config.diff(next_print_config).empty();
    filament_changed = !filament_config.diff(next_filament_config).empty();
    return true;
}

bool apply_object_simulation_advice_to_current_plater(const ObjectSimulationAdvice& advice)
{
    Tab* print_tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
    Tab* filament_tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT);
    Plater* plater = wxGetApp().plater();
    if (print_tab == nullptr || filament_tab == nullptr || print_tab->get_config() == nullptr ||
        filament_tab->get_config() == nullptr || plater == nullptr)
        return false;

    DynamicPrintConfig print_config;
    DynamicPrintConfig filament_config;
    bool print_changed = false;
    bool filament_changed = false;
    if (!prepare_object_simulation_advice_config_change(advice, *print_tab->get_config(), *filament_tab->get_config(),
            print_config, filament_config, print_changed, filament_changed))
        return false;

    if (!print_changed && !filament_changed) {
        plater->schedule_background_process();
        plater->update();
        if (advice.requires_reslice)
            plater->reslice();
        MessageDialog dialog(plater,
            advice.requires_reslice ?
                _L("This recommendation is already in the current settings. Re-slicing has started to update the preview and G-code.") :
                _L("This recommendation is already in the current settings."),
            _L("Object simulation analysis"), wxOK | wxICON_INFORMATION);
        dialog.ShowModal();
        return true;
    }

    print_tab->load_config(print_config);
    filament_tab->load_config(filament_config);
    if (print_changed)
        plater->sidebar().update_presets(Preset::TYPE_PRINT);
    if (filament_changed)
        plater->sidebar().update_presets(Preset::TYPE_FILAMENT);
    plater->schedule_background_process();
    plater->update();
    if (advice.requires_reslice)
        plater->reslice();

    MessageDialog dialog(plater,
        advice.requires_reslice ?
            _L("Object simulation recommendation applied. Re-slicing has started to update the preview and G-code.") :
            _L("Object simulation recommendation applied."),
        _L("Object simulation analysis"), wxOK | wxICON_INFORMATION);
    dialog.ShowModal();
    return true;
}

std::string object_simulation_advice_severity_label(ObjectSimulationAdviceSeverity severity)
{
    switch (severity) {
    case ObjectSimulationAdviceSeverity::High:   return _u8L("High");
    case ObjectSimulationAdviceSeverity::Medium: return _u8L("Medium");
    case ObjectSimulationAdviceSeverity::Low:    return _u8L("Low");
    case ObjectSimulationAdviceSeverity::Info:   return _u8L("Info");
    }
    return _u8L("Info");
}

std::string object_simulation_thermal_resolution_label(int resolution)
{
    switch (resolution) {
    case 0: return _u8L("Fast");
    case 2: return _u8L("High");
    case 1:
    default: return _u8L("Auto");
    }
}

std::string object_simulation_config_key_label(const std::string& key)
{
    if (key == "first_x_layer_fan_speed")       return _u8L("First-layer fan");
    if (key == "fan_min_speed")                 return _u8L("Minimum fan");
    if (key == "fan_max_speed")                 return _u8L("Maximum fan");
    if (key == "additional_cooling_fan_speed")  return _u8L("Auxiliary fan");
    if (key == "chamber_temperature")           return _u8L("Chamber temperature");
    if (key == "hot_plate_temp_initial_layer")  return _u8L("First-layer bed temperature");
    if (key == "hot_plate_temp")                return _u8L("Bed temperature");
    if (key == "brim_type")                     return _u8L("Brim type");
    if (key == "brim_width")                    return _u8L("Brim width");
    if (key == "brim_ears_max_angle")           return _u8L("Mouse-ear angle");
    if (key == "brim_ears_detection_length")    return _u8L("Mouse-ear detection length");
    if (key == "raft_layers")                   return _u8L("Raft layers");
    if (key == "outer_wall_speed")              return _u8L("Outer wall speed");
    if (key == "inner_wall_speed")              return _u8L("Inner wall speed");
    if (key == "initial_layer_speed")           return _u8L("Initial layer speed");
    if (key == "enable_warp_prevention")        return _u8L("Warp prevention");
    if (key == "warp_prevention_level")         return _u8L("Warp prevention level");
    if (key == "warp_prevention_thermal_resolution") return _u8L("Thermal model resolution");
    if (key == "warp_prevention_max_slowdown")  return _u8L("Maximum slowdown");
    if (key == "warp_prevention_min_wall_speed") return _u8L("Minimum wall speed");
    if (key == "warp_prevention_min_bottom_speed") return _u8L("Minimum bottom speed");
    if (key == "warp_prevention_early_layers")  return _u8L("Max active layers");
    if (key == "warp_prevention_adjust_acceleration") return _u8L("Adjust acceleration");
    if (key == "warp_prevention_max_accel_reduction") return _u8L("Maximum acceleration reduction");
    if (key == "warp_prevention_min_wall_acceleration") return _u8L("Minimum wall acceleration");
    if (key == "warp_prevention_min_bottom_acceleration") return _u8L("Minimum bottom acceleration");
    if (key == "filament_shrink")               return _u8L("Material shrinkage");
    if (key == "temperature_vitrification")     return _u8L("Glass transition temperature");
    if (key == "filament_type")                 return _u8L("Material type");
    return key;
}

std::string object_simulation_config_keys_label(const std::vector<std::string>& keys)
{
    std::string out;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i > 0)
            out += ", ";
        out += object_simulation_config_key_label(keys[i]);
    }
    return out.empty() ? std::string("-") : out;
}

} // namespace GUI
} // namespace Slic3r
