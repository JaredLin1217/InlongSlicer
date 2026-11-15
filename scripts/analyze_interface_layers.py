from __future__ import annotations

import json
import math
import re
import sys
from pathlib import Path

E_RE = re.compile(r"\sE(-?(?:\d+(?:\.\d+)?|\.\d+))")
XY_RE = re.compile(r"X(-?(?:\d+(?:\.\d+)?|\.\d+))\s+Y(-?(?:\d+(?:\.\d+)?|\.\d+))")


def parse(path: Path):
    z = 0.0
    role = ""
    current = None
    groups = []
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if line.startswith(";Z:"):
            if current and current["pts"]:
                groups.append(current)
            current = None
            z = float(line[3:])
        if line.startswith(";TYPE:"):
            if current and current["pts"]:
                groups.append(current)
            current = None
            role = line[6:].strip()
        if role != "Support interface":
            continue
        e = E_RE.search(line)
        if e and float(e.group(1)) < 0:
            if current and current["pts"]:
                groups.append(current)
            current = None
            continue
        xy = XY_RE.search(line)
        if not xy or not e or float(e.group(1)) <= 0:
            continue
        if current is None:
            current = {"z": z, "role": role, "pts": []}
        current["pts"].append((float(xy.group(1)), float(xy.group(2))))
    if current and current["pts"]:
        groups.append(current)
    return groups


def summarize(groups):
    rows = []
    for index, group in enumerate(groups):
        pts = group["pts"]
        steps = [math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(pts, pts[1:])]
        rows.append({
            "index": index,
            "z": group["z"],
            "points": len(pts),
            "length_mm": round(sum(steps), 4),
            "max_step_mm": round(max(steps, default=0.0), 4),
            "bbox": [round(min(p[0] for p in pts), 4), round(min(p[1] for p in pts), 4),
                     round(max(p[0] for p in pts), 4), round(max(p[1] for p in pts), 4)],
        })
    return rows


def unique_z(rows):
    return sorted({round(row["z"], 5) for row in rows})


def main():
    ordinary = summarize(parse(Path(sys.argv[1])))
    organic = summarize(parse(Path(sys.argv[2])))
    ordinary_z = unique_z(ordinary)
    organic_z = unique_z(organic)
    missing = [z for z in ordinary_z if not any(abs(z - oz) < 1e-4 for oz in organic_z)]
    organic_gaps = [round(b - a, 5) for a, b in zip(organic_z, organic_z[1:])]
    ordinary_gaps = [round(b - a, 5) for a, b in zip(ordinary_z, ordinary_z[1:])]
    rows = []
    for row in organic:
        refs = [ref for ref in ordinary if abs(ref["z"] - row["z"]) < 0.25]
        if refs:
            ref = min(refs, key=lambda x: abs(x["z"] - row["z"]))
            delta = max(abs(row["bbox"][i] - ref["bbox"][i]) for i in range(4))
            center_delta = math.hypot(
                ((row["bbox"][0] + row["bbox"][2]) - (ref["bbox"][0] + ref["bbox"][2])) / 2,
                ((row["bbox"][1] + row["bbox"][3]) - (ref["bbox"][1] + ref["bbox"][3])) / 2,
            )
            rows.append({**row, "ordinary_z": ref["z"], "bbox_edge_delta_mm": round(delta, 4), "center_delta_mm": round(center_delta, 4)})
        else:
            rows.append({**row, "ordinary_z": None, "bbox_edge_delta_mm": None, "center_delta_mm": None})
    print(json.dumps({
        "ordinary": {"groups": len(ordinary), "unique_layers": len(ordinary_z), "z": ordinary_z, "max_gap_mm": max(ordinary_gaps, default=0)},
        "organic": {"groups": len(organic), "unique_layers": len(organic_z), "z": organic_z, "max_gap_mm": max(organic_gaps, default=0)},
        "ordinary_layers_missing_from_organic": missing,
        "organic_layers": rows,
    }, indent=2))


if __name__ == "__main__":
    main()
