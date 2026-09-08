"""Keep old imported preset references readable after the Inlong identity port."""
import json
import unittest
from pathlib import Path


ARENA = Path(__file__).resolve().parents[2] / 'resources' / 'profiles' / 'InlongArena'


class TestInlongArenaAliases(unittest.TestCase):
    def assert_aliases(self, relative_path, name, aliases):
        profile = json.loads((ARENA / relative_path).read_text(encoding='utf-8-sig'))
        self.assertEqual(profile['name'], name)
        self.assertEqual(profile['instantiation'], 'true')
        actual = profile['renamed_from'].split(';')
        self.assertEqual(len(actual), len(set(actual)))
        for alias in aliases:
            self.assertIn(alias, actual)
        # Legacy branding is allowed only in read-compatibility metadata, never
        # in the canonical name, inheritance, printer model or profile links.
        canonical = {key: value for key, value in profile.items() if key != 'renamed_from'}
        self.assertNotIn('OrcaArena', json.dumps(canonical))
        self.assertNotIn('Orca Arena', json.dumps(canonical))

    def test_old_printer_names_resolve_to_inlong_profiles(self):
        for nozzle in ('0.2', '0.4', '0.6', '0.8'):
            with self.subTest(nozzle=nozzle):
                name = f'Inlong Arena X1 Carbon {nozzle} nozzle'
                self.assert_aliases(Path('machine') / (name + '.json'), name,
                                    [f'Orca Arena X1 Carbon {nozzle} nozzle'])

    def test_both_generations_of_generic_material_names_are_preserved(self):
        materials = ('ABS', 'ASA', 'PA', 'PA-CF', 'PC', 'PETG', 'PLA', 'PLA Silk',
                     'PLA-CF', 'PVA', 'TPU')
        for material in materials:
            with self.subTest(material=material):
                name = f'Generic {material} @InlongArena'
                self.assert_aliases(Path('filament') / (name + '.json'), name, [
                    f'InlongArena Generic {material}', f'OrcaArena Generic {material}',
                    f'Generic {material} @OrcaArena',
                ])

    def test_legacy_nozzle_and_carbon_fiber_material_names_are_preserved(self):
        for material in ('ABS', 'ASA', 'PC', 'PETG', 'PLA', 'PVA'):
            with self.subTest(material=material):
                name = f'Generic {material} @InlongArena 0.2 nozzle'
                self.assert_aliases(Path('filament') / (name + '.json'), name, [
                    f'InlongArena Generic {material} @0.2 nozzle',
                    f'InlongArena Generic {material} 0.2 nozzle',
                    f'OrcaArena Generic {material} @0.2 nozzle',
                    f'OrcaArena Generic {material} 0.2 nozzle',
                    f'Generic {material} @OrcaArena 0.2 nozzle',
                ])
        name = 'Generic PETG-CF @Arena X1C'
        self.assert_aliases(Path('filament') / (name + '.json'), name, [
            'InlongArena Generic PETG-CF @Arena X1C', 'InlongArena Generic PETG-CF Arena X1C',
            'OrcaArena Generic PETG-CF @Arena X1C', 'OrcaArena Generic PETG-CF Arena X1C',
        ])


if __name__ == '__main__':
    unittest.main()
