"""Keep build and install data directories empty without touching other data."""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
CMAKE = shutil.which('cmake')
RESET = ROOT / 'cmake' / 'ResetBuildDataDirs.cmake'
MODULE = ROOT / 'cmake' / 'InlongBuildDataDirs.cmake'
INSTALL_ONLY = ROOT / 'cmake' / 'InstallOnlyInlong.cmake'


@unittest.skipUnless(CMAKE, 'CMake is not available')
class TestBuildDataDirs(unittest.TestCase):
    def setUp(self):
        scratch = ROOT / 'build'
        scratch.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='data-dir-test-', dir=scratch)
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.build = self.root / 'build'
        self.source.mkdir()
        self.build.mkdir()
        (self.build / 'CMakeCache.txt').write_text(
            f'CMAKE_HOME_DIRECTORY:INTERNAL={self.source.as_posix()}\n', encoding='utf-8')

    def run_cmake(self, *args, success=True):
        result = subprocess.run([CMAKE, *map(str, args)], capture_output=True,
                                text=True, errors='replace', timeout=60)
        output = result.stdout + result.stderr
        if success:
            self.assertEqual(result.returncode, 0, output)
        else:
            self.assertNotEqual(result.returncode, 0, output)
        return output

    def reset(self, multi=True, config='Release', **overrides):
        variables = dict(SOURCE_ROOT=self.source, BUILD_ROOT=self.build,
                         SLIC3R_APP_KEY='InlongSlicer', MULTI_CONFIG='TRUE' if multi else 'FALSE',
                         CONFIG=config)
        variables.update(overrides)
        return [f'-D{key}={value}' for key, value in variables.items()] + ['-P', RESET]

    def data_dirs(self, multi=True, config='Release'):
        binary = self.build / 'src'
        if multi:
            binary /= config
        return [binary / 'data_dir', self.build / 'InlongSlicer' / 'data_dir',
                self.build / 'InlongSlicer_InlongOnly' / 'data_dir']

    def populate(self, paths):
        for path in paths:
            (path / 'nested').mkdir(parents=True, exist_ok=True)
            (path / '.settings').write_text('old settings', encoding='utf-8')
            (path / 'nested' / 'cache').write_text('old cache', encoding='utf-8')

    def assert_empty(self, paths):
        for path in paths:
            self.assertTrue(path.is_dir(), str(path))
            self.assertEqual(list(path.iterdir()), [], str(path))

    def test_missing_directories_are_created_for_both_generator_layouts(self):
        for multi in (True, False):
            with self.subTest(multi=multi):
                self.run_cmake(*self.reset(multi=multi))
                self.assert_empty(self.data_dirs(multi=multi))

    def test_repeated_reset_removes_nested_and_hidden_settings_only(self):
        paths = self.data_dirs()
        unrelated = self.build / 'keep.txt'
        unrelated.write_text('keep', encoding='utf-8')
        debug = self.build / 'src' / 'Debug' / 'data_dir'
        self.populate([debug])
        for _ in range(2):
            self.populate(paths)
            self.run_cmake(*self.reset())
            self.assert_empty(paths)
            self.assertEqual(unrelated.read_text(encoding='utf-8'), 'keep')
            self.assertTrue((debug / '.settings').exists())

    def test_source_root_and_wrong_cache_owner_are_rejected_before_removal(self):
        paths = self.data_dirs()
        self.populate(paths)
        self.run_cmake(*self.reset(BUILD_ROOT=self.source), success=False)
        self.run_cmake(*self.reset(SOURCE_ROOT=self.root), success=False)
        for path in paths:
            self.assertTrue((path / '.settings').exists())

    def test_path_traversal_in_application_or_configuration_is_rejected(self):
        paths = self.data_dirs()
        self.populate(paths)
        self.run_cmake(*self.reset(SLIC3R_APP_KEY='../outside'), success=False)
        self.run_cmake(*self.reset(config='../../outside'), success=False)
        for path in paths:
            self.assertTrue((path / '.settings').exists())

    def make_directory_link(self, link, target):
        link.parent.mkdir(parents=True, exist_ok=True)
        if os.name == 'nt':
            # Directory junctions need no Windows symlink privilege.
            result = subprocess.run(['cmd', '/c', 'mklink', '/J', str(link), str(target)],
                                    capture_output=True, text=True, errors='replace', timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            link.symlink_to(target, target_is_directory=True)
        self.addCleanup(link.unlink if os.name != 'nt' else link.rmdir)

    def test_linked_output_parent_cannot_redirect_the_reset(self):
        paths = self.data_dirs()
        self.populate([paths[0]])
        external = self.root / 'external'
        self.populate([external / 'data_dir'])
        self.make_directory_link(self.build / 'InlongSlicer', external)
        self.run_cmake(*self.reset(), success=False)
        self.assertTrue((paths[0] / '.settings').exists())
        self.assertTrue((external / 'data_dir' / '.settings').exists())

    def test_nested_link_is_rejected_before_any_directory_is_cleared(self):
        paths = self.data_dirs()
        self.populate(paths)
        external = self.root / 'external'
        external.mkdir()
        marker = external / 'keep.txt'
        marker.write_text('keep', encoding='utf-8')
        self.make_directory_link(paths[-1] / 'linked', external)
        self.run_cmake(*self.reset(), success=False)
        self.assertEqual(marker.read_text(encoding='utf-8'), 'keep')
        for path in paths:
            self.assertTrue((path / '.settings').exists())

    @unittest.skipUnless(shutil.which('ninja'), 'Ninja is not available')
    def test_incremental_runtime_builds_and_direct_install_always_reset(self):
        (self.source / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.13)
project(DataDirFixture NONE)
set(SLIC3R_APP_KEY InlongSlicer)
set(CMAKE_INSTALL_PREFIX "${{CMAKE_BINARY_DIR}}/InlongSlicer" CACHE PATH "" FORCE)
add_custom_target(InlongSlicer)
add_custom_target(InlongSlicer_app_gui)
add_dependencies(InlongSlicer_app_gui InlongSlicer)
add_custom_target(InlongSlicer_profile_validator)
include("{MODULE.as_posix()}")
''', encoding='utf-8')
        for generator, multi in (('Ninja', False), ('Ninja Multi-Config', True)):
            with self.subTest(generator=generator):
                self.build = self.root / ('build-multi' if multi else 'build-single')
                self.run_cmake('-S', self.source, '-B', self.build, '-G', generator,
                               '-DCMAKE_BUILD_TYPE=Release')
                paths = self.data_dirs(multi=multi)
                for _ in range(2):
                    for target in ('InlongSlicer', 'InlongSlicer_app_gui',
                                   'InlongSlicer_profile_validator'):
                        self.populate(paths)
                        self.run_cmake('--build', self.build, '--config', 'Release',
                                       '--target', target)
                        self.assert_empty(paths)
                self.populate(paths)
                self.run_cmake('--install', self.build, '--config', 'Release')
                self.assert_empty(paths)

    def test_inlong_only_install_outputs_an_empty_data_directory(self):
        profiles = self.source / 'resources' / 'profiles'
        for directory in ('InlongFilamentLibrary', 'INLONG', '_Infinity3DP', 'user'):
            (profiles / directory).mkdir(parents=True)
        for filename in ('InlongFilamentLibrary.json', 'INLONG.json', '_Infinity3DP.json',
                         'blacklist.json', 'hotend.stl'):
            (profiles / filename).write_text('fixture', encoding='utf-8')
        python = self.build / 'src' / 'Release' / 'python'
        python.mkdir(parents=True)
        (python / 'python.exe').write_text('fixture', encoding='utf-8')
        output = self.build / 'InlongSlicer_InlongOnly'
        self.populate([output / 'data_dir'])
        self.run_cmake(f'-DSOURCE_ROOT={self.source}', f'-DBUILD_ROOT={self.build}',
                       '-DCONFIG=Release', '-DSLIC3R_APP_KEY=InlongSlicer',
                       f'-DOUTPUT_DIR={output}', '-P', INSTALL_ONLY)
        self.assert_empty([output / 'data_dir'])


if __name__ == '__main__':
    unittest.main()
