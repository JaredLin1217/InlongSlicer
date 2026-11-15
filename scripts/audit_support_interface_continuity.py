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


def distance(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


if __name__ == "__main__":
    groups = parse(Path(sys.argv[1]))
    support = [g for g in groups if g["role"] == "Support"]
    interfaces = [g for g in groups if g["role"] == "Support interface"]
    rows = []
    for group in interfaces:
        same_z = [g for g in support if abs(g["z"] - group["z"]) < 1e-4]
        support_points = [p for g in same_z for p in g["pts"]]
        nearest = min((distance(p, q) for p in group["pts"] for q in support_points), default=float("inf"))
        rows.append({"z": group["z"], "support_groups": len(same_z), "nearest_support_mm": round(nearest, 4)})
    print(json.dumps({
        "interface_groups": len(interfaces),
        "support_groups": len(support),
        "layers_without_same_z_support": sum(1 for row in rows if row["support_groups"] == 0),
        "max_nearest_support_mm": max((row["nearest_support_mm"] for row in rows), default=0),
        "rows": rows,
    }, indent=2))
