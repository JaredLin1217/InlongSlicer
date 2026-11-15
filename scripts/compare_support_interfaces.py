#!/usr/bin/env python3
"""Compare ordinary and organic support-interface envelopes layer by layer.

The ordinary-support G-code is treated as a geometric reference only.  Organic
layers are matched to the nearest ordinary layer (or linearly interpolated
between the surrounding ordinary layers), while the production slicer remains
independent of G-code files.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
from pathlib import Path


NUMBER = r"-?(?:\d+(?:\.\d+)?|\.\d+)"
XY = re.compile(rf"X({NUMBER})\s+Y({NUMBER})")
E = re.compile(rf"\sE({NUMBER})")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def parse(path: Path) -> dict[float, dict[str, object]]:
    z = 0.0
    role = ""
    width = 0.0
    active: list[tuple[float, float]] = []
    rows: dict[float, list[dict[str, object]]] = {}

    def flush() -> None:
        nonlocal active
        if len(active) < 2:
            active = []
            return
        length = sum(math.dist(active[i - 1], active[i]) for i in range(1, len(active)))
        bbox = [
            min(point[0] for point in active), min(point[1] for point in active),
            max(point[0] for point in active), max(point[1] for point in active),
        ]
        rows.setdefault(z, []).append({"length": length, "bbox": bbox, "points": len(active), "width": width})
        active = []

    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if line.startswith(";Z:"):
            flush()
            z = float(line[3:])
        if line.startswith(";TYPE:"):
            flush()
            role = line[6:].strip()
        if line.startswith(";WIDTH:"):
            width = float(line[7:])
        if role != "Support interface":
            continue
        extrusion = E.search(line)
        if extrusion and float(extrusion.group(1)) < 0:
            flush()
            continue
        if not (line.startswith("G0") or line.startswith("G1")) or not extrusion:
            continue
        if float(extrusion.group(1)) <= 0 or "F" in line:
            continue
        match = XY.search(line)
        if match:
            active.append((float(match.group(1)), float(match.group(2))))
    flush()

    return {
        float(layer_z): {
            "paths": paths,
            "count": len(paths),
            "length": sum(float(item["length"]) for item in paths),
            "bbox": [
                min(float(item["bbox"][0]) for item in paths),
                min(float(item["bbox"][1]) for item in paths),
                max(float(item["bbox"][2]) for item in paths),
                max(float(item["bbox"][3]) for item in paths),
            ],
            "short_paths_below_3_5mm": sum(float(item["length"]) < 3.5 for item in paths),
        }
        for layer_z, paths in rows.items()
    }


def bbox_center(bbox: list[float]) -> tuple[float, float]:
    return ((bbox[0] + bbox[2]) / 2.0, (bbox[1] + bbox[3]) / 2.0)


def interpolate(ordinary: dict[float, dict[str, object]], z: float) -> dict[str, object] | None:
    if not ordinary:
        return None
    zs = sorted(ordinary)
    if z <= zs[0]:
        return ordinary[zs[0]]
    if z >= zs[-1]:
        return ordinary[zs[-1]]
    upper_index = next(index for index, value in enumerate(zs) if value >= z)
    lower_z, upper_z = zs[upper_index - 1], zs[upper_index]
    lower, upper = ordinary[lower_z], ordinary[upper_z]
    ratio = (z - lower_z) / (upper_z - lower_z)
    lower_bbox, upper_bbox = lower["bbox"], upper["bbox"]
    bbox = [a + (b - a) * ratio for a, b in zip(lower_bbox, upper_bbox)]
    return {
        "count": lower["count"] if ratio < 0.5 else upper["count"],
        "length": float(lower["length"]) + (float(upper["length"]) - float(lower["length"])) * ratio,
        "bbox": bbox,
        "short_paths_below_3_5mm": 0,
        "reference_z": [lower_z, upper_z],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("ordinary", type=Path)
    parser.add_argument("organic", type=Path)
    parser.add_argument("--tolerance", type=float, default=0.05)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    ordinary = parse(args.ordinary)
    organic = parse(args.organic)
    layers = []
    for z in sorted(organic):
        actual = organic[z]
        reference = interpolate(ordinary, z)
        if reference is None:
            continue
        actual_center = bbox_center(actual["bbox"])
        reference_center = bbox_center(reference["bbox"])
        center_delta = math.dist(actual_center, reference_center)
        edge_delta = max(
            abs(float(actual["bbox"][i]) - float(reference["bbox"][i])) for i in range(4)
        )
        layers.append({
            "z": z,
            "reference_z": reference.get("reference_z", [z, z]),
            "organic_bbox": actual["bbox"],
            "ordinary_reference_bbox": reference["bbox"],
            "bbox_edge_delta_mm": edge_delta,
            "center_delta_mm": center_delta,
            "organic_interface_paths": actual["count"],
            "organic_interface_length_mm": actual["length"],
            "organic_short_paths": actual["short_paths_below_3_5mm"],
            "aligned_within_tolerance": edge_delta <= args.tolerance,
        })

    result = {
        "ordinary_gcode": str(args.ordinary),
        "organic_gcode": str(args.organic),
        "ordinary_sha256": sha256(args.ordinary),
        "organic_sha256": sha256(args.organic),
        "tolerance_mm": args.tolerance,
        "ordinary_interface_layers": len(ordinary),
        "organic_interface_layers": len(organic),
        "organic_short_path_count": sum(int(item["organic_short_paths"]) for item in layers),
        "layers": layers,
        "summary": {
            "layers_within_tolerance": sum(item["aligned_within_tolerance"] for item in layers),
            "layers_total": len(layers),
            "max_bbox_edge_delta_mm": max((item["bbox_edge_delta_mm"] for item in layers), default=0.0),
            "max_center_delta_mm": max((item["center_delta_mm"] for item in layers), default=0.0),
        },
    }
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
