#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Utils.hpp"
#include "test_helpers.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

const std::string chamber_probes =
    "; RAW_CHAMBER={overall_chamber_temperature}\n";

std::string profile_start_gcode(const std::string &relative)
{
    std::ifstream input(std::string(PROFILES_DIR) + "/" + relative);
    REQUIRE(input.is_open());
    nlohmann::json profile;
    input >> profile;
    return profile.at("machine_start_gcode").get<std::string>();
}

DynamicPrintConfig chamber_config(unsigned int filaments = 1)
{
    DynamicPrintConfig config = multifilament_config(filaments, {
        { "gcode_flavor",               "marlin" },
        { "emit_machine_limits_to_gcode", "0" },
        { "machine_start_gcode",         chamber_probes },
        { "machine_end_gcode",           "" },
        { "layer_height",                "0.2" },
        { "initial_layer_print_height",  "0.2" },
        { "initial_layer_line_width",    "0" },
        { "skirt_loops",                 "0" },
        { "min_skirt_length",            "0" },
        { "enable_prime_tower",          "0" },
        { "ooze_prevention",             "0" },
        { "print_sequence",              "by object" },
        { "support_chamber_temp_control", "1" },
        { "activate_chamber_temp_control", "1" },
        { "chamber_temperature",         "80" },
    });
    // Match the full per-filament variant columns supplied by PresetBundle,
    // including nullable overrides absent from the generic full-print defaults.
    for (const std::string &key : filament_options_with_variant) {
        auto *option = config.option(key, true);
        REQUIRE(option != nullptr);
        REQUIRE(option->is_vector());
        static_cast<ConfigOptionVectorBase *>(option)->resize(filaments, print_config_def.get(key)->default_value.get());
    }
    if (filaments == 2) {
        // Exercise two physical nozzles as well as two logical material slots.
        config.set_num_extruders(2);
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4}));
        config.set_key_value("printer_extruder_id", new ConfigOptionInts({1, 2}));
        config.set_key_value("printer_extruder_variant",
                             new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard"}));
        config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(fmmManual));
        config.set_key_value("filament_map", new ConfigOptionInts({1, 2}));
        config.set_key_value("filament_self_index", new ConfigOptionInts({1, 2}));
        config.set_key_value("filament_extruder_variant",
                             new ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard"}));
        config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    }
    return config;
}

std::string slice_chamber_job(const DynamicPrintConfig &config, const std::vector<int> &used_filaments = {1})
{
    const std::string previous_resources = resources_dir();
    const ScopeGuard restore_resources([previous_resources] { set_resources_dir(previous_resources); });
    set_resources_dir(boost::filesystem::path(PROFILES_DIR).parent_path().string());
    std::vector<TriangleMesh> meshes;
    std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides;
    std::vector<unsigned int> expected_extruders;
    for (size_t i = 0; i < used_filaments.size(); ++i) {
        TriangleMesh mesh = cube(4.);
        // Keep the fixture on a finite bed and clear of the preceding object.
        mesh.translate(float(20. + i * 40.), 20.f, 0.f);
        meshes.emplace_back(std::move(mesh));
        overrides.push_back({{"extruder", std::to_string(used_filaments[i])}});
        expected_extruders.push_back(unsigned(used_filaments[i] - 1));
    }
    Print print;
    Model model;
    init_print(std::move(meshes), print, model, config, &overrides, false);
    REQUIRE(print.extruders() == expected_extruders);
    return gcode(print);
}

struct ChamberCommand {
    std::string code;
    float temperature;
};

std::vector<ChamberCommand> chamber_commands(const std::string &gcode)
{
    std::vector<ChamberCommand> commands;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &, const GCodeReader::GCodeLine &line) {
        if (line.cmd_is("M141") || line.cmd_is("M191")) {
            float temperature = 0.f;
            REQUIRE(line.has_value('S', temperature));
            commands.push_back({std::string(line.cmd()), temperature});
        }
    });
    return commands;
}

void check_profile_chamber_cycle(const std::string &gcode, int target, bool automatic_shutdown)
{
    const auto commands = chamber_commands(gcode);
    const size_t expected_count = 1 + (target > 0 ? 1 : 0) + (automatic_shutdown ? 1 : 0);
    for (const auto &command : commands)
        INFO(command.code + " S" + std::to_string(command.temperature));
    REQUIRE(commands.size() == expected_count);
    CHECK(commands.front().code == "M141");
    CHECK_THAT(commands.front().temperature, Catch::Matchers::WithinAbs(target, 0.001));
    if (target > 0) {
        CHECK(commands[1].code == "M191");
        CHECK_THAT(commands[1].temperature, Catch::Matchers::WithinAbs(target, 0.001));
        // Preserve concurrent chamber/bed heating, then wait for the chamber.
        const auto heat = gcode.find("\nM141 ");
        const auto bed_wait = gcode.rfind("\nM190 ", gcode.find("\nM191 "));
        const auto chamber_wait = gcode.find("\nM191 ");
        REQUIRE(bed_wait != std::string::npos);
        CHECK(heat < bed_wait);
        CHECK(bed_wait < chamber_wait);
    }
    if (automatic_shutdown) {
        CHECK(commands.back().code == "M141");
        CHECK_THAT(commands.back().temperature, Catch::Matchers::WithinAbs(0., 0.001));
    }
}

} // namespace

TEST_CASE("Profile chamber commands suppress the original automatic wait", "[ChamberTemperature][Profiles][Regression]")
{
    const auto flavor = GENERATE(gcfMarlinLegacy, gcfKlipper);
    CAPTURE(flavor);
    auto config = chamber_config();
    config.set_key_value("gcode_flavor", new ConfigOptionEnum<GCodeFlavor>(flavor));
    config.set_key_value("support_chamber_temp_control", new ConfigOptionBool(false));
    // Prove this test uses the unchanged core: without profile commands it
    // automatically waits for 80 despite the printer switch being off.
    const auto original_commands = chamber_commands(slice_chamber_job(config));
    REQUIRE(original_commands.size() == 2);
    CHECK(original_commands[0].code == "M191");
    CHECK_THAT(original_commands[0].temperature, Catch::Matchers::WithinAbs(80., 0.001));
    CHECK(original_commands[1].code == "M141");
    CHECK_THAT(original_commands[1].temperature, Catch::Matchers::WithinAbs(0., 0.001));
    config.set_key_value("machine_start_gcode", new ConfigOptionString(
        profile_start_gcode("INLONG/machine/Vulcan600_common.json")));
    check_profile_chamber_cycle(slice_chamber_job(config), 0, true);
}

TEST_CASE("Shipped chamber startup templates respect both control switches", "[ChamberTemperature][Profiles]")
{
    const std::string profile = GENERATE(
        "INLONG/machine/SC12060_common.json",
        "INLONG/machine/Vulcan600_common.json",
        "INLONG/machine/Vulcan1200_common.json",
        "_Infinity3DP/machine/Infinity3DP IXBOX common.json",
        "_Infinity3DP/machine/Infinity3DP IXBOX DUO common.json",
        "_Infinity3DP/machine/Infinity3DP X1 common.json",
        "_Infinity3DP/machine/Infinity3DP X2 common.json",
        "_Infinity3DP/machine/Infinity3DP X2 DUO common.json",
        "_Infinity3DP/machine/Infinity3DP X3 common.json",
        "_Infinity3DP/machine/Infinity3DP X600HD common.json");
    const bool supported = GENERATE(false, true);
    const bool enabled = GENERATE(false, true);
    const int temperature = GENERATE(0, 80);
    const auto flavor = GENERATE(gcfMarlinLegacy, gcfKlipper);
    CAPTURE(profile, supported, enabled, temperature, flavor);
    auto config = chamber_config();
    config.set_key_value("gcode_flavor", new ConfigOptionEnum<GCodeFlavor>(flavor));
    config.set_key_value("machine_start_gcode", new ConfigOptionString(profile_start_gcode(profile)));
    config.set_key_value("support_chamber_temp_control", new ConfigOptionBool(supported));
    config.set_key_value("activate_chamber_temp_control", new ConfigOptionBools({enabled}));
    config.set_key_value("chamber_temperature", new ConfigOptionInts({temperature}));
    check_profile_chamber_cycle(slice_chamber_job(config), supported && enabled ? temperature : 0,
                                enabled && temperature > 0);
}

TEST_CASE("Dual-nozzle profile chamber targets ignore disabled used materials", "[ChamberTemperature][Profiles][MultiFilament]")
{
    const auto sequence = GENERATE(PrintSequence::ByLayer, PrintSequence::ByObject);
    const std::string profile = GENERATE(
        "INLONG/machine/Vulcan600_common.json",
        "INLONG/machine/Vulcan1200_common.json",
        "_Infinity3DP/machine/Infinity3DP IXBOX DUO common.json",
        "_Infinity3DP/machine/Infinity3DP X2 DUO common.json",
        "_Infinity3DP/machine/Infinity3DP X3 common.json",
        "_Infinity3DP/machine/Infinity3DP X600HD common.json");
    const bool supported = GENERATE(false, true);
    const bool first_enabled = GENERATE(false, true);
    const bool second_enabled = GENERATE(false, true);
    CAPTURE(profile, supported, first_enabled, second_enabled, sequence);
    auto config = chamber_config(2);
    config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(sequence));
    config.set_key_value("support_chamber_temp_control", new ConfigOptionBool(supported));
    config.set_key_value("activate_chamber_temp_control", new ConfigOptionBools({first_enabled, second_enabled}));
    config.set_key_value("chamber_temperature", new ConfigOptionInts({40, 80}));
    config.set_key_value("machine_start_gcode", new ConfigOptionString(chamber_probes + profile_start_gcode(profile)));
    const int expected = supported ? std::max(first_enabled ? 40 : 0, second_enabled ? 80 : 0) : 0;
    const std::string output = slice_chamber_job(config, {1, 2});
    check_profile_chamber_cycle(output, expected, first_enabled || second_enabled);
    CHECK_THAT(output, Catch::Matchers::ContainsSubstring("; RAW_CHAMBER=80\n"));
}

TEST_CASE("Unused materials do not activate or raise profile chamber heating", "[ChamberTemperature][Profiles][MultiFilament]")
{
    const int used_slot = GENERATE(0, 1);
    const bool enabled = GENERATE(false, true);
    CAPTURE(used_slot, enabled);
    auto config = chamber_config(2);
    auto *switches = new ConfigOptionBools({true, true});
    switches->values[used_slot] = enabled;
    config.set_key_value("activate_chamber_temp_control", switches);
    auto *temperatures = new ConfigOptionInts({120, 120});
    temperatures->values[used_slot] = 40;
    config.set_key_value("chamber_temperature", temperatures);
    config.set_key_value("machine_start_gcode", new ConfigOptionString(
        chamber_probes + profile_start_gcode("INLONG/machine/Vulcan600_common.json")));
    const std::string output = slice_chamber_job(config, {used_slot + 1});
    check_profile_chamber_cycle(output, enabled ? 40 : 0, enabled);
    CHECK_THAT(output, Catch::Matchers::ContainsSubstring("; RAW_CHAMBER=40\n"));
}

TEST_CASE("An enabled zero chamber target cannot inherit a disabled material temperature", "[ChamberTemperature][Profiles][Regression]")
{
    const int enabled_slot = GENERATE(0, 1);
    CAPTURE(enabled_slot);
    auto config = chamber_config(2);
    auto *switches = new ConfigOptionBools({false, false});
    switches->values[enabled_slot] = true;
    config.set_key_value("activate_chamber_temp_control", switches);
    auto *temperatures = new ConfigOptionInts({120, 120});
    temperatures->values[enabled_slot] = 0;
    config.set_key_value("chamber_temperature", temperatures);
    config.set_key_value("machine_start_gcode", new ConfigOptionString(
        profile_start_gcode("INLONG/machine/Vulcan600_common.json")));
    // The unchanged core may still append its heater-off command, but the
    // disabled material must not cause a positive target or any chamber wait.
    check_profile_chamber_cycle(slice_chamber_job(config, {1, 2}), 0, true);
}
