"""Check the shipped INLONG and Infinity3DP profile contracts without user data."""
import json
import re
import unittest
from pathlib import Path


PROFILES = Path(__file__).resolve().parents[2] / 'resources/profiles'
BRANDS = ('INLONG', '_Infinity3DP')
FANS = {
    'PLA': ('100', '70', '100'), 'PETG-CFGF': ('100', '60', '80'),
    'PET-CFGF': ('70', '30', '35'), 'ABS': ('80', '15', '25'),
    'TPU': ('100', '30', '50'), 'PC': ('50', '10', '25'),
    'PA6': ('80', '15', '25'), 'PATH-CFGF': ('50', '10', '20'),
    'PPA-CFGF': ('50', '10', '20'), 'PPS-CFGF': ('50', '15', '20'),
    'PAEK': ('30', '0', '10'), 'PEEK': ('30', '0', '10'),
    'HIPS': ('75', '0', '10'), 'VXL': ('75', '0', '10'),
}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'Duplicate JSON key: {key}')
        result[key] = value
    return result


def load_vendor(brand):
    manifest = json.loads((PROFILES / f'{brand}.json').read_bytes(), object_pairs_hook=unique_object)
    profiles = {}
    for kind in ('machine', 'filament', 'process'):
        for entry in manifest[f'{kind}_list']:
            path = PROFILES / brand / entry['sub_path']
            data = json.loads(path.read_bytes(), object_pairs_hook=unique_object)
            if data['name'] != entry['name'] or data['type'] != kind:
                raise ValueError(f'Manifest identity mismatch: {path}')
            if data['name'] in profiles:
                raise ValueError(f'Duplicate profile: {data["name"]}')
            profiles[data['name']] = data
    return manifest, profiles


def resolve(profiles, name, stack=()):
    if name in stack:
        raise ValueError(f'Inheritance cycle: {stack + (name,)}')
    data = profiles[name]
    base = resolve(profiles, data['inherits'], stack + (name,)) if data.get('inherits') else {}
    return dict(base, **data)


class TestInlongProfileGcode(unittest.TestCase):
    def setUp(self):
        self.vendors = {brand: load_vendor(brand) for brand in BRANDS}

    def printers(self):
        for brand, (_, profiles) in self.vendors.items():
            for name, data in profiles.items():
                if data['type'] == 'machine' and data.get('instantiation') == 'true':
                    yield brand, name, resolve(profiles, name)

    def test_all_declared_profiles_resolve_without_cycles(self):
        for brand, (_, profiles) in self.vendors.items():
            for name in profiles:
                with self.subTest(brand=brand, profile=name):
                    self.assertEqual(resolve(profiles, name)['name'], name)

    def test_printer_defaults_and_compatibility_references_exist(self):
        for brand, name, printer in self.printers():
            profiles = self.vendors[brand][1]
            with self.subTest(printer=name):
                process = resolve(profiles, printer['default_print_profile'])
                self.assertEqual(process['type'], 'process')
                self.assertIn(name, process['compatible_printers'])
                for material in printer['default_filament_profile']:
                    filament = resolve(profiles, material)
                    self.assertEqual(filament['type'], 'filament')
                    self.assertIn(name, filament['compatible_printers'])
        for brand, (_, profiles) in self.vendors.items():
            for name, raw in profiles.items():
                if raw.get('instantiation') != 'true' or raw['type'] == 'machine':
                    continue
                with self.subTest(brand=brand, preset=name):
                    for printer in resolve(profiles, name).get('compatible_printers', []):
                        self.assertEqual(profiles[printer]['type'], 'machine')

    def test_material_fan_values_match_the_approved_table(self):
        keys = ('filament_heatbreak_fan_speed', 'fan_min_speed', 'fan_max_speed')
        for brand, (_, profiles) in self.vendors.items():
            prefix = 'INLONG' if brand == 'INLONG' else 'Infinity3DP'
            for material, expected in FANS.items():
                with self.subTest(brand=brand, material=material):
                    actual = resolve(profiles, f'{prefix} {material}')
                    self.assertEqual(tuple(actual[key] for key in keys), expected)
                    self.assertLessEqual(int(actual['fan_min_speed']), int(actual['fan_max_speed']))

    def test_startup_retraction_uses_resolved_non_nullable_speed(self):
        for _, name, printer in self.printers():
            with self.subTest(printer=name):
                start = printer['machine_start_gcode']
                self.assertIsNone(re.search(r'filament_retraction_speed\s*\[', start),
                                  'Startup must not read a nullable material override directly')
                self.assertIn('retraction_speed[0]*60', start)
                self.assertIn('retraction_speed[1]*60', start)

    def test_global_heatbreak_fan_follows_each_incoming_material(self):
        expression = 'M710 S{round(filament_heatbreak_fan_speed[next_extruder] * 255.0 / 100.0)}'
        for _, name, printer in self.printers():
            if printer.get('heatbreak_fan_control_mode') != 'global_m710':
                continue
            with self.subTest(printer=name):
                change = printer['change_filament_gcode']
                self.assertEqual(change.count(expression), 1)
                self.assertLess(change.index('T{next_extruder}'), change.index(expression))
                # Put the fan command directly after selecting the new tool, before purge moves.
                after_tool = change.split('T{next_extruder}', 1)[1].splitlines()
                self.assertEqual(after_tool[1], expression)

    def test_chamber_startup_uses_the_printer_and_material_control_target(self):
        guard = '{if inlong_chamber_target > 0}'
        for _, name, printer in self.printers():
            with self.subTest(printer=name):
                start = printer['machine_start_gcode']
                self.assertIn('{local inlong_chamber_target = 0}', start)
                self.assertIn('{if support_chamber_temp_control}', start)
                self.assertIn('{if size(chamber_temperature) > 1 and '
                              'size(activate_chamber_temp_control) > 1}', start)
                for slot in (0, 1):
                    self.assertIn(f'{{if (is_extruder_used[{slot}] or initial_extruder == {slot}) and '
                                  f'activate_chamber_temp_control[{slot}]}}', start)
                    self.assertIn('{inlong_chamber_target = max(inlong_chamber_target, '
                                  f'chamber_temperature[{slot}])}}', start)
                self.assertEqual(start.count('M141 S{inlong_chamber_target}'), 1)
                self.assertEqual(start.count(guard), 1)
                self.assertIn(guard + '\nM191 S{inlong_chamber_target}\n{endif}', start)
                self.assertNotIn('controlled_chamber_temperature', start)
                self.assertNotIn('overall_chamber_temperature', start)

    def test_heatbreak_pwm_conversion_does_not_truncate_integer_division(self):
        for _, name, printer in self.printers():
            if printer.get('heatbreak_fan_control_mode') != 'global_m710':
                continue
            with self.subTest(printer=name):
                for key in ('machine_start_gcode', 'change_filament_gcode'):
                    expressions = re.findall(r'round\(filament_heatbreak_fan_speed\[[^]]+\][^)]*\)', printer[key])
                    self.assertTrue(expressions)
                    for expression in expressions:
                        self.assertIn('* 255.0 / 100.0', expression)

    def test_custom_heatbreak_mode_does_not_gain_an_m710_command(self):
        for _, name, printer in self.printers():
            if printer.get('heatbreak_fan_control_mode') != 'custom_gcode':
                continue
            with self.subTest(printer=name):
                self.assertNotRegex(printer['machine_start_gcode'], r'(?m)^M710\b')
                self.assertNotRegex(printer['change_filament_gcode'], r'(?m)^M710\b')
                self.assertEqual(printer['heatbreak_fan_gcode_template'], '')

    def test_declared_nozzle_variants_preserve_machine_topology(self):
        counts = {'Vulcan600': 2, 'Vulcan1200': 2, 'SC12060': 1,
                  'Infinity3DP IXBOX': 1, 'Infinity3DP X1': 1, 'Infinity3DP X2': 1,
                  'Infinity3DP IXBOX DUO': 2, 'Infinity3DP X2 DUO': 2,
                  'Infinity3DP X3': 2, 'Infinity3DP X600HD': 2}
        for _, name, printer in self.printers():
            with self.subTest(printer=name):
                self.assertEqual(len(printer['nozzle_diameter']), counts[printer['printer_model']])
                self.assertTrue(all(float(value) == float(printer['printer_variant'])
                                    for value in printer['nozzle_diameter']))


if __name__ == '__main__':
    unittest.main()
