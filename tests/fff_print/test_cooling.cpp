#include <catch2/catch_all.hpp>

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/GCode/FanMover.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

TEST_CASE("Short fan kick-starts retain their duration after fan advance", "[Cooling][Regression]")
{
    const float lead = GENERATE(0.f, 0.5f);
    const bool relative_e = GENERATE(false, true);
    const bool separate_chunks = GENERATE(false, true);
    constexpr float kickstart = 0.1f;
    GCodeWriter writer;
    writer.config.gcode_flavor.value = gcfKlipper;
    writer.config.use_relative_e_distances.value = relative_e;
    writer.config.part_cooling_fan_min_pwm.value = 0;
    FanMover mover(writer, lead, true, relative_e, false, kickstart);

    const std::string prefix = std::string("G90\n") + (relative_e ? "M83\n" : "M82\n") +
        "G1 X0 Y0 Z0 F600\nM106 S0\n;TYPE:Outer wall\nG1 X10 E1 F600\nG1 X20 E" +
        (relative_e ? "1" : "2") + " F600\nM106 S153\n";
    const std::string suffix = std::string("G1 X30 E") + (relative_e ? "1" : "3") +
        " F600\nG1 X40 E" + (relative_e ? "1" : "4") + " F600\nM106 S0\nG1 X50 E" +
        (relative_e ? "1" : "5") + " F600\n";
    std::string output;
    if (separate_chunks) {
        output = mover.process_gcode(prefix, false);
        output += mover.process_gcode(suffix, true);
    } else {
        output = mover.process_gcode(prefix + suffix, true);
    }

    GCodeReader reader;
    reader.apply_config(writer.config);
    double elapsed = 0., extrusion = 0., distance = 0.;
    double full_at = -1., target_at = -1.;
    double feedrate = 600.;
    reader.parse_buffer(output, [&](GCodeReader &parser, const GCodeReader::GCodeLine &line) {
        if (line.has_f())
            feedrate = line.f();
        if (line.cmd_is("G1")) {
            const double length = std::sqrt(std::pow(line.dist_X(parser), 2) +
                                            std::pow(line.dist_Y(parser), 2) + std::pow(line.dist_Z(parser), 2));
            elapsed += length / (feedrate / 60.);
            distance += length;
            extrusion += relative_e ? line.e() : line.dist_E(parser);
        } else if (line.cmd_is("M106")) {
            float pwm = -1.f;
            line.has_value('S', pwm);
            if (pwm == 255.f)
                full_at = elapsed;
            else if (pwm == 153.f)
                target_at = elapsed;
        }
    });
    REQUIRE(full_at >= 0.);
    REQUIRE(target_at >= 0.);
    CHECK_THAT(full_at, Catch::Matchers::WithinAbs(2. - lead, 0.0002));
    // The configured kick-start is scaled by the 0 -> 60% speed increase.
    CHECK_THAT(target_at - full_at, Catch::Matchers::WithinAbs(kickstart * 0.6, 0.0002));
    CHECK_THAT(distance, Catch::Matchers::WithinAbs(50., 0.0002));
    CHECK_THAT(extrusion, Catch::Matchers::WithinAbs(5., 0.00002));
    CHECK_THAT(reader.x(), Catch::Matchers::WithinAbs(50., 0.0002));
}

TEST_CASE("A fan kick-start ends at an exact move boundary", "[Cooling][Regression]")
{
    GCodeWriter writer;
    writer.config.gcode_flavor.value = gcfKlipper;
    writer.config.use_relative_e_distances.value = true;
    FanMover mover(writer, 0.f, true, true, false, 0.125f);
    // S128 is parsed as 50%; its 0.0625 s kick-start ends at X20.625.
    const std::string output = mover.process_gcode(
        "G90\nM83\nG1 X0 Y0 Z0 F600\nM106 S0\nG1 X20 E2\nM106 S128\n"
        "G1 X20.625 E0.0625\nG1 X30 E0.9375\n", true);
    const size_t boundary = output.find("G1 X20.625");
    const size_t restored = output.find("M106 S128");
    const size_t following = output.find("G1 X30");
    REQUIRE(boundary != std::string::npos);
    REQUIRE(restored != std::string::npos);
    REQUIRE(following != std::string::npos);
    CHECK(boundary < restored);
    CHECK(restored < following);
}

// The fan is held off for the first close_fan_the_first_x_layers layers, so an explicit
// fan-off command is emitted.
TEST_CASE("Fan is held off for the initial layers", "[Cooling]")
{
    const std::string gcode = slice({ cube(20) }, {
        { "cooling",                      true },
        { "close_fan_the_first_x_layers", 5 },
    });
    CHECK(gcode.find("M106 S0") != std::string::npos);
}

// The cooling pass resolves and strips its internal speed placeholders; none leak into
// the final G-code.
TEST_CASE("Cooling consumes its internal speed markers", "[Cooling]")
{
    const std::string gcode = slice({ cube(20) }, { { "layer_height", 0.2 } });
    CHECK(gcode.find(";_EXTRUDE_SET_SPEED") == std::string::npos);
}

TEST_CASE("Arc fitting remains active with overhang fan control", "[Cooling][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_arc_fitting",             true },
        { "enable_overhang_bridge_fan",     true },
        { "enable_overhang_speed",          true },
        { "initial_layer_print_height",     0.2 },
        { "layer_height",                   0.2 },
        { "slow_down_for_layer_cooling",    false },
    });
    config.set_key_value("fan_max_speed", new ConfigOptionFloats{20.0});
    config.set_key_value("fan_min_speed", new ConfigOptionFloats{20.0});
    config.set_key_value("overhang_fan_speed", new ConfigOptionInts{100});
    config.set_key_value("overhang_fan_threshold", new ConfigOptionEnumsGeneric{Overhang_threshold_2_4});
    config.set_key_value("layer_change_gcode", new ConfigOptionString{";TEST_LAYER=[layer_num]"});

    const auto has_arc_after_first_layer = [](const std::string &gcode) {
        std::istringstream input(gcode);
        std::string line;
        size_t layers_seen = 0;
        while (std::getline(input, line)) {
            if (line.rfind(";TEST_LAYER=", 0) == 0)
                ++layers_seen;
            else if (layers_seen > 1 && (line.rfind("G2 ", 0) == 0 || line.rfind("G3 ", 0) == 0))
                return true;
        }
        return false;
    };
    const auto has_arc_with_overhang_fan = [](const std::string &gcode) {
        std::istringstream input(gcode);
        std::string line;
        bool overhang_fan_active = false;
        bool outer_wall = false;
        while (std::getline(input, line)) {
            if (line.rfind("M106 S255", 0) == 0)
                overhang_fan_active = true;
            else if (line.rfind("M106 S0", 0) == 0 || line.rfind("M107", 0) == 0)
                overhang_fan_active = false;
            else if (line.rfind(";TYPE:", 0) == 0)
                outer_wall = line == ";TYPE:Outer wall";
            else if (overhang_fan_active && outer_wall && (line.rfind("G2 ", 0) == 0 || line.rfind("G3 ", 0) == 0))
                return true;
        }
        return false;
    };

    const std::string with_overhang_speed = slice({ make_cylinder(25.0, 10.0, 2.0 * PI / 72.0) }, config);
    CHECK(has_arc_after_first_layer(with_overhang_speed));

    config.set_deserialize_strict({ { "enable_overhang_speed", false } });
    const std::string without_overhang_speed = slice({ make_cylinder(25.0, 10.0, 2.0 * PI / 72.0) }, config);
    CHECK(has_arc_after_first_layer(without_overhang_speed));

    const std::string uniform_overhang_gcode = slice({ make_sphere(50.0, 2.0 * PI / 72.0) }, config);
    CHECK(has_arc_with_overhang_fan(uniform_overhang_gcode));
}

TEST_CASE("Overhang fan transitions do not depend on overhang speed", "[Cooling][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "bridge_speed",                   2.0 },
        { "enable_arc_fitting",             false },
        { "enable_overhang_bridge_fan",     true },
        { "enable_overhang_speed",          false },
        { "initial_layer_print_height",     0.3 },
        { "inner_wall_speed",              30.0 },
        { "layer_height",                   0.3 },
        { "outer_wall_speed",              30.0 },
        { "overhang_1_4_speed",            "30" },
        { "overhang_2_4_speed",            "29" },
        { "overhang_3_4_speed",             "6" },
        { "overhang_4_4_speed",             "3" },
        { "slow_down_for_layer_cooling",    false },
        { "slowdown_for_curled_perimeters", false },
    });
    config.set_key_value("fan_max_speed", new ConfigOptionFloats{20.0});
    config.set_key_value("fan_min_speed", new ConfigOptionFloats{20.0});
    config.set_key_value("overhang_fan_speed", new ConfigOptionInts{100});
    config.set_key_value("overhang_fan_threshold", new ConfigOptionEnumsGeneric{Overhang_threshold_2_4});
    config.set_key_value("layer_change_gcode", new ConfigOptionString{";TEST_LAYER_Z=[layer_z]"});

    const auto fan_commands = [](const std::string &gcode) {
        std::vector<std::pair<std::string, std::string>> commands;
        std::istringstream input(gcode);
        std::string layer;
        std::string line;
        while (std::getline(input, line)) {
            if (line.rfind(";TEST_LAYER_Z=", 0) == 0)
                layer = line;
            else if (!layer.empty() && (line.rfind("M106", 0) == 0 || line.rfind("M107", 0) == 0))
                commands.emplace_back(layer, line);
        }
        return commands;
    };
    const auto feedrates = [](const std::string &gcode) {
        std::vector<std::string> values;
        std::istringstream input(gcode);
        std::string word;
        while (input >> word)
            if (!word.empty() && word.front() == 'F')
                values.push_back(word);
        return values;
    };

    constexpr double sphere_radius = 50.0; // 100 mm diameter.
    const std::string without_speed_gcode = slice({make_sphere(sphere_radius, PI / 24.0)}, config);
    config.set_deserialize_strict({{"enable_overhang_speed", true}});
    const std::string with_speed_gcode = slice({make_sphere(sphere_radius, PI / 24.0)}, config);

    const auto without_speed_fan = fan_commands(without_speed_gcode);
    const auto with_speed_fan = fan_commands(with_speed_gcode);
    const auto without_speed_feedrates = feedrates(without_speed_gcode);
    const auto with_speed_feedrates = feedrates(with_speed_gcode);

    REQUIRE_FALSE(without_speed_fan.empty());
    REQUIRE(std::any_of(without_speed_fan.begin(), without_speed_fan.end(),
                        [](const auto &command) { return command.second.find("S255") != std::string::npos; }));
    REQUIRE(with_speed_feedrates != without_speed_feedrates);
    CHECK(with_speed_fan == without_speed_fan);
}
