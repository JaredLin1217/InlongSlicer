#!/usr/bin/env python3
"""Parse InlongSlicer timing logs without changing G-code or source outputs."""
import argparse
import json
import re
from pathlib import Path

NUMBER = r"([0-9]+(?:\.[0-9]+)?)"
PATTERNS = [
    ("node_placement", re.compile(r"Organic node placement profiling:\s*" + NUMBER + r" seconds")),
    ("draw_smoothing", re.compile(r"Organic draw profiling: smoothing\s*" + NUMBER + r" seconds")),
    ("draw_rasterization", re.compile(r"Organic draw profiling: rasterization\s*" + NUMBER + r" seconds")),
    ("draw_tree_layer_unions", re.compile(r"Organic draw profiling: tree-layer unions\s*" + NUMBER + r" seconds")),
    ("draw_final_polygon_assembly", re.compile(r"Organic draw profiling: final polygon assembly\s*" + NUMBER + r" seconds")),
    ("interface_generation", re.compile(r"Organic interface profiling:\s*" + NUMBER + r" seconds")),
    ("source_anchoring", re.compile(r"Organic source anchoring:\s*" + NUMBER + r" seconds")),
    ("physical_height_modulation", re.compile(r"Organic physical-height modulation:\s*" + NUMBER + r" seconds")),
    ("printed_anchoring", re.compile(r"Organic printed anchoring:\s*" + NUMBER + r" seconds")),
    ("interface_joining", re.compile(r"Organic interface joining:\s*" + NUMBER + r" seconds")),
    ("path_validation", re.compile(r"Organic path validation:.*?anchoring\s*" + NUMBER + r" seconds, joining\s*" + NUMBER + r" seconds, bracing\s*" + NUMBER + r" seconds")),
    ("tree_support_total_ms", re.compile(r"Total time used creating Tree support.*?:\s*" + NUMBER + r" ms")),
]


def parse(log_path: Path, process_path: Path | None = None) -> dict:
    text = log_path.read_text(encoding="utf-8-sig", errors="replace")
    phases = {}
    matches = {}
    for name, pattern in PATTERNS:
        found = pattern.findall(text)
        if not found:
            continue
        if name == "path_validation":
            value = {"anchoring": float(found[-1][0]), "joining": float(found[-1][1]), "bracing": float(found[-1][2])}
        else:
            value = float(found[-1])
            if name == "tree_support_total_ms":
                value /= 1000.0
        phases[name] = value
        matches[name] = len(found)
    progress = re.findall(r"Organic printed anchoring progress: layer=(\d+)/(\d+), components=(\d+), local overlaps=(\d+), certified=(\d+), indexes=(\d+)", text)
    result = {"log": log_path.as_posix(), "phases_seconds": phases, "occurrences": matches,
              "printed_anchoring_progress": {
                  "last_layer": int(progress[-1][0]) if progress else None,
                  "plan_layers": int(progress[-1][1]) if progress else None,
                  "components": int(progress[-1][2]) if progress else None,
                  "local_overlaps": int(progress[-1][3]) if progress else None,
                  "certified": int(progress[-1][4]) if progress else None,
                  "indexes": int(progress[-1][5]) if progress else None,
              }}
    if process_path and process_path.exists():
        process = json.loads(process_path.read_text(encoding="utf-8-sig"))
        result["process"] = {key: process.get(key) for key in (
            "exit_code", "timed_out", "elapsed_seconds", "cpu_seconds",
            "peak_working_set_bytes", "peak_private_bytes", "data_unchanged", "gcode_sha256")}
        result["complete_export"] = process.get("exit_code") == 0 and not process.get("timed_out") and bool(process.get("gcode_sha256"))
    observed = []
    for key, value in phases.items():
        if isinstance(value, (float, int)):
            observed.append(float(value))
        elif isinstance(value, dict):
            observed.extend(float(part) for part in value.values())
    result["observed_phase_seconds"] = sum(observed)
    result["largest_observed_phase"] = max(phases.items(), key=lambda item: max(item[1].values()) if isinstance(item[1], dict) else item[1], default=None)
    if "process" in result and result["process"].get("elapsed_seconds") is not None:
        result["unattributed_elapsed_seconds"] = max(0.0, float(result["process"]["elapsed_seconds"]) - result["observed_phase_seconds"])
        result["unattributed_phase_note"] = "For an incomplete export this is a lower bound for unlogged path validation, final toolpath work, and shutdown; it is not assigned to one stage without a completed export."
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--process", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = parse(args.log, args.process)
    encoded = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")


if __name__ == "__main__":
    main()
