from __future__ import annotations

import json
import re
import sys
from pathlib import Path

E_RE = re.compile(r"\sE(-?(?:\d+(?:\.\d+)?|\.\d+))")
XY_RE = re.compile(r"X(-?(?:\d+(?:\.\d+)?|\.\d+))\s+Y(-?(?:\d+(?:\.\d+)?|\.\d+))")


def parse(path: Path):
    z = 0.0
    role = ""
    group = None
    groups = []
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if line.startswith(";Z:"):
            if group and group["pts"]:
                groups.append(group)
            group = None
            z = float(line[3:])
        if line.startswith(";TYPE:"):
            if group and group["pts"]:
                groups.append(group)
            group = None
            role = line[6:].strip()
        if role != "Support interface":
            continue
        e = E_RE.search(line)
        if e and float(e.group(1)) < 0:
            if group and group["pts"]:
                groups.append(group)
            group = None
            continue
        xy = XY_RE.search(line)
        if not xy or not e or float(e.group(1)) <= 0:
            continue
        if group is None:
            group = {"z": z, "pts": []}
        group["pts"].append((float(xy.group(1)), float(xy.group(2))))
    if group and group["pts"]:
        groups.append(group)
    out = []
    for index, item in enumerate(groups):
        pts = item["pts"]
        length = sum(((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2) ** 0.5 for a, b in zip(pts, pts[1:]))
        max_step = max((((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2) ** 0.5 for a, b in zip(pts, pts[1:])), default=0.0)
        out.append({
            "index": index,
            "z": item["z"],
            "points": len(pts),
            "length_mm": round(length, 4),
            "max_step_mm": round(max_step, 4),
            "bbox": [round(min(p[0] for p in pts), 4), round(min(p[1] for p in pts), 4),
                     round(max(p[0] for p in pts), 4), round(max(p[1] for p in pts), 4)],
        })
    return out


if __name__ == "__main__":
    groups = parse(Path(sys.argv[1]))
    print(json.dumps({"groups": len(groups), "short": [g for g in groups if g["length_mm"] < 1.0], "all": groups}, indent=2))
