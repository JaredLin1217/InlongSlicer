#include "WarpPrevention.hpp"

#include <algorithm>
#include <cctype>

namespace Slic3r {
namespace {

static std::string uppercase_ascii(std::string value)
{
    for (char& c : value)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return value;
}

static bool has_token(const std::string& value, const char* token)
{
    return value.find(token) != std::string::npos;
}

static void configure_material_card(WarpPreventionMaterial& profile,
                                    float conductivity_w_m_k,
                                    float specific_heat_j_kg_k,
                                    float density_kg_m3,
                                    float bonding_temperature,
                                    float shrinkage_percent,
                                    float confidence)
{
    profile.thermal_profile.conductivity_w_m_k = conductivity_w_m_k;
    profile.thermal_profile.specific_heat_j_kg_k = specific_heat_j_kg_k;
    profile.thermal_profile.density_kg_m3 = density_kg_m3;
    profile.thermal_profile.thermal_expansion = profile.thermal_expansion;
    profile.thermal_profile.elastic_modulus_gpa = profile.elastic_modulus_gpa;
    profile.thermal_profile.bonding_temperature = bonding_temperature;
    profile.thermal_profile.softening_temperature = profile.softening_temperature;
    profile.thermal_profile.shrinkage_percent = shrinkage_percent;
    profile.thermal_profile.confidence = confidence;
}

} // namespace

WarpPreventionMaterial make_warp_prevention_material(const std::string& filament_type)
{
    WarpPreventionMaterial profile;
    const std::string type = uppercase_ascii(filament_type);

    if (has_token(type, "PEEK") || has_token(type, "PEKK") || has_token(type, "PAEK") ||
        has_token(type, "PPSU") || has_token(type, "PPS") || has_token(type, "ULTEM") ||
        has_token(type, "PEI")) {
        profile.material_sensitivity = 1.0f;
        profile.thermal_expansion = 48.0e-6f;
        profile.elastic_modulus_gpa = 3.5f;
        profile.softening_temperature = 150.0f;
        profile.recommended_chamber_temperature = 130.0f;
        profile.recommended_bed_temperature = 130.0f;
        profile.high_warp_material = true;
        profile.material_profile_known = true;
        configure_material_card(profile, 0.25f, 1340.0f, 1300.0f, 300.0f, 0.70f, 0.72f);
        return profile;
    }

    if (has_token(type, "PC")) {
        profile.material_sensitivity = 0.90f;
        profile.thermal_expansion = 68.0e-6f;
        profile.elastic_modulus_gpa = 2.3f;
        profile.softening_temperature = 115.0f;
        profile.recommended_chamber_temperature = 80.0f;
        profile.recommended_bed_temperature = 110.0f;
        profile.high_warp_material = true;
        profile.material_profile_known = true;
        configure_material_card(profile, 0.20f, 1200.0f, 1200.0f, 135.0f, 0.60f, 0.70f);
        return profile;
    }

    if (has_token(type, "ABS") || has_token(type, "ASA")) {
        profile.material_sensitivity = 0.85f;
        profile.thermal_expansion = 85.0e-6f;
        profile.elastic_modulus_gpa = 2.0f;
        profile.softening_temperature = 100.0f;
        profile.recommended_chamber_temperature = 60.0f;
        profile.recommended_bed_temperature = 100.0f;
        profile.high_warp_material = true;
        profile.material_profile_known = true;
        configure_material_card(profile, 0.18f, 1300.0f, 1050.0f, 110.0f, 0.80f, 0.68f);
        return profile;
    }

    if (type == "PA" || has_token(type, "PA-") || has_token(type, "PA6") ||
        has_token(type, "PA12") || has_token(type, "PAHT") || has_token(type, "PPA")) {
        profile.material_sensitivity = 0.82f;
        profile.thermal_expansion = 90.0e-6f;
        profile.elastic_modulus_gpa = 1.8f;
        profile.softening_temperature = 85.0f;
        profile.recommended_chamber_temperature = 60.0f;
        profile.recommended_bed_temperature = 90.0f;
        profile.high_warp_material = true;
        profile.material_profile_known = true;
        configure_material_card(profile, 0.25f, 1700.0f, 1130.0f, 95.0f, 0.75f, 0.62f);
        return profile;
    }

    if (has_token(type, "PET-CF") || has_token(type, "PET-GF") || has_token(type, "PETG-CF") ||
        has_token(type, "PETG-GF") || has_token(type, "PCTG")) {
        profile.material_sensitivity = 0.62f;
        profile.thermal_expansion = 55.0e-6f;
        profile.elastic_modulus_gpa = 2.4f;
        profile.softening_temperature = 80.0f;
        profile.recommended_chamber_temperature = 45.0f;
        profile.recommended_bed_temperature = 80.0f;
        profile.material_profile_known = true;
        configure_material_card(profile, 0.30f, 1200.0f, 1350.0f, 85.0f, 0.35f, 0.58f);
        return profile;
    }

    if (has_token(type, "PETG") || has_token(type, "PET")) {
        profile.material_sensitivity = 0.35f;
        profile.thermal_expansion = 65.0e-6f;
        profile.elastic_modulus_gpa = 2.0f;
        profile.softening_temperature = 75.0f;
        profile.recommended_chamber_temperature = 35.0f;
        profile.recommended_bed_temperature = 75.0f;
        profile.material_profile_known = true;
        configure_material_card(profile, 0.22f, 1250.0f, 1270.0f, 75.0f, 0.28f, 0.58f);
        return profile;
    }

    if (has_token(type, "PLA")) {
        profile.material_sensitivity = 0.25f;
        profile.thermal_expansion = 68.0e-6f;
        profile.elastic_modulus_gpa = 3.0f;
        profile.softening_temperature = 60.0f;
        profile.recommended_chamber_temperature = 30.0f;
        profile.recommended_bed_temperature = 55.0f;
        profile.material_profile_known = true;
        configure_material_card(profile, 0.13f, 1800.0f, 1240.0f, 60.0f, 0.20f, 0.62f);
        return profile;
    }

    if (has_token(type, "TPU") || has_token(type, "PVA")) {
        profile.material_sensitivity = 0.25f;
        profile.thermal_expansion = 95.0e-6f;
        profile.elastic_modulus_gpa = 0.8f;
        profile.softening_temperature = 60.0f;
        profile.recommended_chamber_temperature = 30.0f;
        profile.recommended_bed_temperature = 55.0f;
        profile.material_profile_known = true;
        configure_material_card(profile, 0.20f, 1800.0f, 1200.0f, 55.0f, 0.35f, 0.50f);
    }

    if (!profile.material_profile_known) {
        configure_material_card(profile, profile.thermal_profile.conductivity_w_m_k,
                                profile.thermal_profile.specific_heat_j_kg_k,
                                profile.thermal_profile.density_kg_m3,
                                profile.softening_temperature,
                                0.25f * std::max(0.2f, profile.material_sensitivity),
                                0.30f);
    }
    return profile;
}

WarpPreventionPreset warp_prevention_preset(WarpPreventionPresetLevel level)
{
    switch (level) {
    case WarpPreventionPresetLevel::Conservative:
        return { 25.0f, 18.0f, 18.0f, 6, true, 40.0f, 1600.0f, 1300.0f };
    case WarpPreventionPresetLevel::Aggressive:
        return { 60.0f, 8.0f, 8.0f, 30, true, 85.0f, 650.0f, 500.0f };
    case WarpPreventionPresetLevel::Balanced:
    default:
        return { 40.0f, 14.0f, 14.0f, 15, true, 70.0f, 1000.0f, 800.0f };
    }
}

} // namespace Slic3r
