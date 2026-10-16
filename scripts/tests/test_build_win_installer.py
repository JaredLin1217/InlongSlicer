"""Exercise the Windows installer's public CLI without building or installing."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(os.name == "nt", "Windows batch CLI")
class TestWindowsInstallerCli(unittest.TestCase):
    def dry_run(self, target):
        with tempfile.TemporaryDirectory(prefix="inlong-installer-cli-") as temporary:
            return subprocess.run(
                ["cmd.exe", "/d", "/c", str(ROOT / "build_win.bat"),
                 "--dry-run", "--installer", "--slicer-target", target,
                 "--config", "Release", "--vs", "2026", "--build-dir", temporary],
                cwd=ROOT, capture_output=True, text=True, errors="replace", timeout=60)

    def test_installer_accepts_the_inlong_executable_target(self):
        result = self.dry_run("InlongSlicer")
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        self.assertIn("--target InlongSlicer", output)
        self.assertIn("cpack -G NSIS", output)
        self.assertIn("InlongSlicer_Windows_Installer_", output)
        self.assertNotIn("OrcaSlicer", output)

    def test_installer_rejects_library_and_old_product_targets_before_building(self):
        for target in ("libslic3r", "OrcaSlicer"):
            with self.subTest(target=target):
                result = self.dry_run(target)
                output = result.stdout + result.stderr
                self.assertNotEqual(result.returncode, 0, output)
                self.assertIn("--installer needs the executable", output)
                self.assertNotIn("+ cpack", output)
                self.assertNotIn("+ cmake --build", output)


if __name__ == "__main__":
    unittest.main()
