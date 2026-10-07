"""CLI tests resolve the Inlong executable and the discovered Python interpreter."""
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CMAKE = shutil.which('cmake')
CTEST = shutil.which('ctest')


@unittest.skipUnless(CMAKE and CTEST, 'CMake and CTest are not available')
class TestCliCMake(unittest.TestCase):
    def test_all_cli_tests_use_the_inlong_target_and_discovered_python(self):
        # Configure the real CLI test directory without requiring the full app
        # or a Linux compiler. An imported executable exercises TARGET_FILE
        # resolution just like the actual InlongSlicer target.
        with tempfile.TemporaryDirectory(prefix='inlong-cli-cmake-') as temporary:
            source = Path(temporary) / 'source'
            build = Path(temporary) / 'build'
            source.mkdir()
            (source / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.13)
project(InlongCliTestFixture NONE)
enable_testing()
add_executable(InlongSlicer IMPORTED)
set_target_properties(InlongSlicer PROPERTIES IMPORTED_LOCATION "${{CMAKE_COMMAND}}")
# CTest omits unavailable commands from its JSON. Resolve bash to an existing
# executable for this configure-only fixture, even on Windows without bash.
add_executable(bash IMPORTED)
set_target_properties(bash PROPERTIES IMPORTED_LOCATION "${{CMAKE_COMMAND}}")
add_subdirectory("{(ROOT / 'tests' / 'cli').as_posix()}" cli)
''', encoding='utf-8')
            result = subprocess.run(
                [CMAKE, '-S', str(source), '-B', str(build),
                 f'-DINLONG_CLI_TEST_PYTHON={Path(sys.executable).as_posix()}'],
                capture_output=True, text=True, errors='replace', timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            result = subprocess.run(
                [CTEST, '--test-dir', str(build), '-C', 'Release', '--show-only=json-v1'],
                capture_output=True, text=True, errors='replace', timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            tests = json.loads(result.stdout)['tests']
            expected = {
                'cli_strict_mode': 'test_cli_strict.sh',
                'cli_project_missing_keys': 'test_cli_project_missing_keys.sh',
                'cli_malformed_input': 'test_cli_malformed_input.sh',
            }
            self.assertEqual({test['name'] for test in tests}, set(expected))
            for test in tests:
                with self.subTest(test=test['name']):
                    command = test['command']
                    self.assertEqual(Path(command[2]).resolve(), Path(CMAKE).resolve())
                    self.assertEqual(Path(command[3]).resolve(), Path(sys.executable).resolve())
                    self.assertEqual(Path(command[1]).name, expected[test['name']])
                    properties = {prop['name']: prop['value'] for prop in test['properties']}
                    self.assertIn('RequiresApp', properties['LABELS'])
                    self.assertEqual(properties['SKIP_RETURN_CODE'], 77)
                    self.assertEqual(properties['TIMEOUT'], 900)


if __name__ == '__main__':
    unittest.main()
