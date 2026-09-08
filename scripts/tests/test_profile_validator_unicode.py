"""Exercise the built validator's UTF-8 command-line and enumerated path handling.

Set INLONG_PROFILE_VALIDATOR to the executable to enable these integration tests.
All profiles are disposable fixtures; no installed or user profiles are opened.
"""
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


VALIDATOR = os.environ.get('INLONG_PROFILE_VALIDATOR', '')


@unittest.skipUnless(VALIDATOR, 'INLONG_PROFILE_VALIDATOR is not set')
class TestProfileValidatorUnicode(unittest.TestCase):
    def validate_fixture(self, root_name):
        with tempfile.TemporaryDirectory(prefix='inlong-validator-') as temporary:
            root = Path(temporary) / root_name
            system = root / 'InlongTestVendor' / 'process'
            user = root / 'user' / 'default' / 'process'
            system.mkdir(parents=True)
            user.mkdir(parents=True)
            fixtures = {
                root / 'InlongTestVendor.json': {
                    'version': '1.0.0', 'name': 'Inlong Test Vendor',
                    'process_list': [{'name': 'Unicode Base', 'sub_path': 'process/base.json'}],
                },
                system / 'base.json': {
                    'type': 'process', 'name': 'Unicode Base', 'from': 'system',
                    'instantiation': 'true', 'layer_height': '0.2',
                },
                user / 'D\u00e9sactiv\u00e9_\u6d4b\u8bd5.json': {
                    'type': 'process', 'name': 'Unicode User', 'from': 'User',
                    'inherits': 'Unicode Base', 'version': '1.0.0',
                },
            }
            for path, profile in fixtures.items():
                path.write_text(json.dumps(profile), encoding='utf-8')
            before = {path: path.read_bytes() for path in fixtures}
            result = subprocess.run([str(Path(VALIDATOR).resolve()), '-p', str(root),
                                     '-v', 'InlongTestVendor', '-l', '2'],
                                    capture_output=True, timeout=60)
            output = (result.stdout + result.stderr).decode('utf-8', errors='replace')
            self.assertEqual(result.returncode, 0, output)
            self.assertIn('Validation completed successfully', output)
            self.assertIn('Total loaded vendors: 1', output)
            for path, content in before.items():
                self.assertEqual(path.read_bytes(), content, str(path))

    def test_unicode_filename_under_an_ascii_root_is_read(self):
        self.validate_fixture('profiles')

    def test_unicode_root_argument_and_filename_are_read(self):
        self.validate_fixture('profiles-\u6d4b\u8bd5-\u00e9')


if __name__ == '__main__':
    unittest.main()
