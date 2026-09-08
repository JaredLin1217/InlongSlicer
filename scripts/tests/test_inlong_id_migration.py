"""Pin the approved INLONG product identities without changing material names."""
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import inlong_id_tool as ids


class TestInlongProductIds(unittest.TestCase):
    def test_shipped_products_match_the_approved_migration(self):
        expected = {
            'ABS': 'OFNmFHz4', 'HIPS': 'OFsJHymG', 'PA6': 'OFODz6Al',
            'PAEK': 'OFQBAaL7', 'PATH-CFGF': 'OFNKQ4ON', 'PC': 'OFY9FEcg',
            'PEEK': 'OFvBDEKj', 'PET-CFGF': 'OFpMcYY6', 'PETG-CFGF': 'OFG7cUEc',
            'PLA': 'OFP4H4J2', 'PPA-CFGF': 'OFRktLCL', 'PPS-CFGF': 'OFG0tstI',
            'TPU': 'OFOr9TlK', 'VXL': 'OFXqccCC',
        }
        folder = Path(ids.PROFILES_DIR) / 'INLONG' / 'filament'
        snapshot = json.loads(Path(ids.SNAPSHOT_PATH).read_text(encoding='utf-8'))['ids']
        for material, filament_id in expected.items():
            name = 'INLONG ' + material
            with self.subTest(material=name):
                profile = json.loads((folder / (name + '.json')).read_text(encoding='utf-8-sig'))
                self.assertEqual(profile['name'], name)
                self.assertEqual(profile['filament_id'], filament_id)
                self.assertEqual(ids.generate_filament_id(profile['filament_vendor'][0],
                                                         profile['filament_type'][0], name), filament_id)
                self.assertIn('INLONG/' + name, snapshot[filament_id]['filaments'])


if __name__ == '__main__':
    unittest.main()
