#ifndef slic3r_GUI_ObjectSimulationAnalysis_hpp_
#define slic3r_GUI_ObjectSimulationAnalysis_hpp_

#include "libslic3r/ObjectSimulationField.hpp"

namespace libvgcode {
struct GCodeInputData;
}

namespace Slic3r {
namespace GUI {

struct ObjectSimulationConfig
{
    int                   analysis_layers{ 1 };
    WarpThermalResolution thermal_resolution{ WarpThermalResolution::Auto };
    float                 material_sensitivity{ 0.45f };
    float                 environment_influence{ 0.35f };
    bool                  high_warp_material{ false };
    bool                  has_brim{ false };
    bool                  has_raft{ false };
};

void assign_object_simulation_display_risk(libvgcode::GCodeInputData& data, const ObjectSimulationConfig& config);

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ObjectSimulationAnalysis_hpp_
