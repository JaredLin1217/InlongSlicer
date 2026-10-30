import importlib.util
import json
import math
from pathlib import Path
import tempfile
import unittest


try:
    spec = importlib.util.spec_from_file_location(
        "analyze_organic_support", Path(__file__).resolve().parents[1] / "analyze_organic_support.py")
    analyzer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(analyzer)
except ModuleNotFoundError as error:
    if error.name != "shapely":
        raise
    analyzer = None


@unittest.skipIf(analyzer is None, "The optional support analyzer requires Shapely")
class SupportAnalysisTests(unittest.TestCase):
    def test_absolute_extrusion_partial_coordinates_and_comments_preserve_a_closed_path(self):
        source = """G90
M82
G92 E0
;Z:0.2
;TYPE:Support
;WIDTH:0.4
;HEIGHT:0.2
G0 X0 Y0
G1 X1 E1
;WIDTH:0.4
G1 Y1 E2
G1 X0 E3
G1 Y0 E4
G1 E3.7
G1 E4
G0 X5
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "plate.gcode"
            path.write_text(source, encoding="utf-8")
            paths, planes, errors = analyzer.read_paths(path)
        self.assertEqual(planes, [0.2])
        self.assertFalse(errors)
        self.assertEqual(len(paths), 1)
        self.assertEqual(paths[0]["points"], [(0., 0.), (1., 0.), (1., 1.), (0., 1.), (0., 0.)])
        self.assertAlmostEqual(sum(paths[0]["segments"]), 4.)

    def test_relative_full_circle_retains_its_arc_length(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "plate.gcode"
            path.write_text("M83\n;Z:1.54\n;TYPE:Support interface\nG0 X2 Y0\nG2 X2 Y0 I1 J0 E0.4\n", encoding="utf-8")
            paths, _, _ = analyzer.read_paths(path)
        self.assertEqual(len(paths), 1)
        self.assertAlmostEqual(sum(paths[0]["segments"]), 2. * math.pi)
        self.assertEqual(paths[0]["points"][0], paths[0]["points"][-1])

    def test_bottom_contact_uses_an_overlapping_model_surface(self):
        def rectangle(x):
            return [[[x, 0.], [x + 1., 0.], [x + 1., 1.], [x, 1.]]]
        row = {"stage": "toolpath_input", "model": [
            {"z": 1.2, "polygons": rectangle(0.)}, {"z": 1.4, "polygons": rectangle(10.)}],
            "groups": {"bottom": [{"z": 1.54, "height": 0.17, "polygons": rectangle(0.)}]}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_text(json.dumps(row) + "\n", encoding="utf-8")
            result = analyzer.geometry_evidence(path)
        contact = result["objects"][0]["bottom_contacts"][0]
        self.assertAlmostEqual(contact["model_top_z"], 1.2)
        self.assertAlmostEqual(contact["gap_mm"], 0.17)

    def test_top_contact_reports_the_original_model_layer_and_missing_xy_demand(self):
        row = {"stage": "toolpath_input", "model": [{"z": 3., "height": 0.2,
            "polygons": [[[0., 0.], [1., 0.], [1., 1.], [0., 1.]]]}],
            "groups": {"bottom": [], "top": [{"z": 2.63, "model_above": 0,
                "polygons": [[[2., 0.], [3., 0.], [3., 1.], [2., 1.]]]}]}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "trace.jsonl"
            path.write_text(json.dumps(row) + "\n", encoding="utf-8")
            result = analyzer.geometry_evidence(path)
        contact = result["objects"][0]["top_contacts"][0]
        self.assertEqual(contact["model_layer_above"], 0)
        self.assertAlmostEqual(contact["gap_mm"], 0.17)
        self.assertAlmostEqual(contact["xy_overlap_mm2"], 0.)


if __name__ == "__main__":
    unittest.main()
