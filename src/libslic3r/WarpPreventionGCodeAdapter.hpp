#ifndef slic3r_WarpPreventionGCodeAdapter_hpp_
#define slic3r_WarpPreventionGCodeAdapter_hpp_

#include "ExtrusionEntity.hpp"
#include "WarpPrevention.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class GCodeWriter;
struct PrintConfig;

struct WarpPreventionPathContext
{
    const PrintConfig* config{ nullptr };
    std::string filament_type;
    uint32_t layer_id{ 0 };
    float layer_height{ 0.0f };
    float line_width{ 0.0f };
    float flow_mm3_per_mm{ 0.0f };
    float z{ 0.0f };
    float path_length_mm{ 0.0f };
    float path_x{ 0.0f };
    float path_y{ 0.0f };
    bool  layer_bounds_valid{ false };
    float layer_min_x{ 0.0f };
    float layer_min_y{ 0.0f };
    float layer_max_x{ 0.0f };
    float layer_max_y{ 0.0f };
    float speed_mm_s{ 0.0f };
    float acceleration_mm_s2{ 0.0f };
    float nozzle_temperature{ 0.0f };
    float chamber_temperature{ 0.0f };
    float bed_temperature{ 0.0f };
    float fan_speed{ 0.0f };
    float filament_shrink_percent{ 100.0f };
    ExtrusionRole role{ erNone };
    bool has_brim{ false };
    bool has_raft{ false };
    bool has_inner_outer_brim{ false };
    float brim_width{ 0.0f };
};

struct WarpPreventionSegmentDecision
{
    WarpPreventionSegment segment;
    double final_speed_mm_s{ 0.0 };
    unsigned int final_acceleration_mm_s2{ 0 };
    std::string fan_gcode;
    std::string acceleration_gcode;
};

class WarpPreventionGCodeAdapter
{
public:
    void reset_print();

    bool begin_path(const WarpPreventionPathContext& context, unsigned int initial_acceleration);
    bool available() const { return m_available; }

    float segment_corner_influence(double prev_x, double prev_y,
                                   double current_x, double current_y,
                                   double next_x, double next_y) const;
    float corner_influence_at(double distance_from_start, double length_mm,
                              float start_corner, float end_corner,
                              double path_width) const;
    std::vector<double> corner_split_params(double length_mm,
                                            float start_turn,
                                            float end_turn,
                                            bool split_wall_corners,
                                            double path_width) const;
    std::vector<double> process_field_split_params(double length_mm,
                                                   bool split_process_field,
                                                   double path_width) const;
    WarpPreventionSegmentDecision process_subsegment(double original_speed_mm_s,
                                                     unsigned int original_acceleration,
                                                     double ax, double ay,
                                                     double bx, double by,
                                                     double length_mm,
                                                     float corner_influence,
                                                     float corner_proximity,
                                                     GCodeWriter& writer,
                                                     double jerk);
    WarpPreventionSegmentDecision process_segment(double original_speed_mm_s,
                                                  unsigned int original_acceleration,
                                                  const WarpPreventionSegment& segment,
                                                  GCodeWriter& writer,
                                                  double jerk);

private:
    struct FanOutputState
    {
        bool valid{ false };
        double current_fan_speed{ 0.0 };
    };

    struct ProcessState
    {
        float process_weight{ 0.0f };
        float local_weight{ 0.0f };
    };

    bool can_adjust_speed(const WarpPreventionSegment& segment) const;
    bool can_adjust_fan(const WarpPreventionSegment& segment) const;
    bool can_adjust_accel(const WarpPreventionSegment& segment) const;

    WarpPreventionSegment make_subsegment(double ax, double ay, double bx, double by, double length_mm, float corner_influence) const;
    WarpPreventionSegment make_subsegment(double ax, double ay, double bx, double by, double length_mm, float corner_influence, float corner_proximity) const;
    double cap_speed(double candidate_speed, const WarpPreventionSegment& source_segment, ProcessState* out_state);
    unsigned int cap_acceleration(unsigned int original_accel, const WarpPreventionSegment& source_segment, const ProcessState& state);
    std::string fan_gcode_if_needed(GCodeWriter& writer, const ProcessState& state, const WarpPreventionSegment& segment);
    std::string acceleration_gcode_if_needed(GCodeWriter& writer, unsigned int target_accel, double jerk);

private:
    WarpPreventionSettings m_settings;
    WarpPreventionMaterial m_material;
    WarpPreventionSegment m_base_segment;
    FanOutputState m_fan_output;
    ExtrusionRole m_role{ erNone };
    unsigned int m_last_set_acceleration{ 0 };
    bool m_available{ false };
};

} // namespace Slic3r

#endif // slic3r_WarpPreventionGCodeAdapter_hpp_
