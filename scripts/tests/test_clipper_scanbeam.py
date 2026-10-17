import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "cmake" / "InlongClipperScanbeam.cmake"
REFERENCE = ROOT / "deps_src" / "clipper2" / "Clipper2Lib" / "src" / "clipper.engine.cpp"
CMAKE = shutil.which("cmake")


@unittest.skipUnless(CMAKE, "CMake is required")
class ClipperScanbeamTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="clipper-scanbeam-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.source = self.directory / "reference.cpp"
        self.output = self.directory / "generated" / "clipper.engine.cpp"
        self.marker = self.directory / "prepared.txt"

    def prepare(self, output=None):
        script = self.directory / "prepare.cmake"
        script.write_text(
            f'include([=[{MODULE.as_posix()}]=])\n'
            f'inlong_prepare_clipper_scanbeam([=[{self.source.as_posix()}]=] '
            f'[=[{(output or self.output).as_posix()}]=] prepared)\n'
            f'file(WRITE [=[{self.marker.as_posix()}]=] "${{prepared}}")\n',
            encoding="utf-8",
        )
        return subprocess.run([CMAKE, "-P", str(script)], capture_output=True, text=True)

    def test_reviewed_source_supports_both_line_endings_without_changing_the_reference(self):
        original = REFERENCE.read_text(encoding="utf-8").replace("\r\n", "\n")
        for newline in ("\n", "\r\n"):
            with self.subTest(newline=repr(newline)):
                raw = original.replace("\n", newline).encode("utf-8")
                self.source.write_bytes(raw)
                result = self.prepare()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.marker.read_text(), "TRUE")
                self.assertEqual(self.source.read_bytes(), raw)
                generated = self.output.read_text(encoding="utf-8")
                self.assertIn("if (already_ordered) return false;", generated)
                self.assertIn("right->curr_x < left->curr_x", generated)

    def test_unknown_engine_revision_keeps_the_original_implementation(self):
        raw = REFERENCE.read_bytes() + b"\n// A different upstream revision.\n"
        self.source.write_bytes(raw)
        result = self.prepare()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.marker.read_text(), "FALSE")
        self.assertFalse(self.output.exists())
        self.assertEqual(self.source.read_bytes(), raw)

    def test_noop_configuration_preserves_the_generated_source_timestamp(self):
        self.source.write_bytes(REFERENCE.read_bytes())
        self.assertEqual(self.prepare().returncode, 0)
        before = self.output.stat().st_mtime_ns
        digest = hashlib.sha256(self.output.read_bytes()).hexdigest()
        self.assertEqual(self.prepare().returncode, 0)
        self.assertEqual(self.output.stat().st_mtime_ns, before)
        self.assertEqual(hashlib.sha256(self.output.read_bytes()).hexdigest(), digest)

    def test_preparation_refuses_to_overwrite_the_reference_source(self):
        raw = REFERENCE.read_bytes()
        self.source.write_bytes(raw)
        result = self.prepare(output=self.source)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must not overwrite its reference source", result.stderr)
        self.assertEqual(self.source.read_bytes(), raw)


if __name__ == "__main__":
    unittest.main()
