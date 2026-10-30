#!/usr/bin/env python3
"""Inspect unmodified organic-support G-code and optional generator geometry.

Requires Shapely. Spacing is measured between distinct closed trunk outlines,
not every infill stroke. Joined outlines are reported as joined components;
their hidden tree-node spacing must be inspected in the source trace. Bottom
gaps require an INLONG_ORGANIC_STAGE_TRACE capture from the same slice. Without
it, this tool leaves the physical-contact measurement unavailable.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import statistics
import zipfile
from collections import Counter, defaultdict
from pathlib import Path

from shapely.geometry import Polygon
from shapely.ops import unary_union


ROLES = ("Support", "Support interface", "Brim")
WORDS = re.compile(r"([XYZEIJ])([-+\d.]+(?:[eE][-+]?\d+)?)")
ERROR = re.compile(r"\b(?:slicing error|landing failed|exception)\b|\berror:", re.I)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def spacing(points):
    if len(points) < 3:
        return None
    distances = [min(math.dist(p, q) for j, q in enumerate(points) if j != i)
                 for i, p in enumerate(points)]
    mean = statistics.mean(distances)
    return {"count": len(points), "mean_mm": mean,
            "stdev_mm": statistics.pstdev(distances),
            "cv": statistics.pstdev(distances) / mean if mean else None,
            "min_mm": min(distances), "max_mm": max(distances)}


def read_paths(path):
    xy, e, relative_e, relative_xy = (0., 0.), 0., False, False
    z, role, width, height = None, "", None, None
    paths, planes, markers = [], [], []
    active = None

    def flush():
        nonlocal active
        if active and len(active["points"]) > 1:
            paths.append(active)
        active = None

    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(";Z:"):
            flush()
            z = float(line[3:])
            planes.append(z)
        elif line.startswith(";TYPE:"):
            flush()
            role = line[6:].strip()
        elif line.startswith(";WIDTH:"):
            width = float(line[7:])
        elif line.startswith(";HEIGHT:"):
            height = float(line[8:])
        if ERROR.search(line):
            markers.append(line[:300])
        command = line.split(";", 1)[0].strip()
        if not command:
            continue
        cmd = command.split()[0]
        words = {k: float(v) for k, v in WORDS.findall(command)}
        if cmd == "M83":
            relative_e = True
        elif cmd == "M82":
            relative_e = False
        elif cmd == "G90":
            relative_xy = False
        elif cmd == "G91":
            relative_xy = True
        elif cmd == "G92":
            e = words.get("E", e)
            xy = words.get("X", xy[0]), words.get("Y", xy[1])
        elif cmd in ("G0", "G1", "G2", "G3"):
            point = tuple(xy[i] + words.get(k, 0.) if relative_xy else words.get(k, xy[i])
                          for i, k in enumerate(("X", "Y")))
            de = words.get("E", 0.) if relative_e else words.get("E", e) - e
            if "E" in words:
                e = e + words["E"] if relative_e else words["E"]
            is_arc = cmd in ("G2", "G3") and ("I" in words or "J" in words)
            if de > 0. and z is not None and (point != xy or is_arc):
                key = (z, role, width, height)
                if active is None or active["key"] != key:
                    flush()
                    active = {"key": key, "z": z, "role": role, "width": width,
                              "height": height, "points": [xy], "segments": []}
                if is_arc:
                    center = xy[0] + words.get("I", 0.), xy[1] + words.get("J", 0.)
                    radius = math.dist(xy, center)
                    a = math.atan2(xy[1] - center[1], xy[0] - center[0])
                    b = math.atan2(point[1] - center[1], point[0] - center[0])
                    sweep = (b - a) % math.tau if cmd == "G3" else -((a - b) % math.tau)
                    if point == xy:
                        sweep = math.tau if cmd == "G3" else -math.tau
                    length = abs(sweep) * radius
                    count = max(2, math.ceil(length / 0.02))
                    active["points"].extend((center[0] + radius * math.cos(a + sweep * i / count),
                                             center[1] + radius * math.sin(a + sweep * i / count))
                                            for i in range(1, count + 1))
                    active["points"][-1] = point
                else:
                    length = math.dist(xy, point)
                    active["points"].append(point)
                active["segments"].append(length)
            else:
                flush()
            xy = point
    flush()
    return paths, sorted(set(planes)), markers


def polygon_area(rings):
    positive, negative = [], []
    for ring in rings:
        if len(ring) < 3:
            continue
        signed = sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(ring, ring[1:] + ring[:1]))
        (positive if signed > 0 else negative).append(Polygon(ring).buffer(0))
    return unary_union(positive).difference(unary_union(negative))


def components(area):
    if area.is_empty:
        return []
    return list(area.geoms) if hasattr(area, "geoms") else [area]


def geometry_evidence(trace):
    rows = [json.loads(line) for line in Path(trace).read_text(encoding="utf-8").splitlines()]
    result = {"source_trace": str(trace), "sha256": digest(trace), "objects": [], "node_layers": []}
    for row in rows:
        if row["stage"] != "nodes":
            continue
        by_z = defaultdict(dict)
        for tree_id, branches in enumerate(row["trees"]):
            for branch in branches:
                for x, y, z, radius, parents, target_layer in branch["points"]:
                    by_z[z][(tree_id, x, y)] = {"tree": tree_id, "x": x, "y": y,
                        "radius": radius, "parents": parents, "target_layer": target_layer}
        result["node_layers"].append([{"z": z, "nodes": list(points.values())} for z, points in sorted(by_z.items())])
    for row in rows:
        if row["stage"] != "toolpath_input":
            continue
        model = [(layer["z"], polygon_area(layer["polygons"])) for layer in row["model"]]
        top_contacts = []
        for layer in row["groups"].get("top", []):
            model_idx = layer.get("model_above")
            entry = {"interface_z": layer["z"], "model_layer_above": model_idx}
            if model_idx is not None and 0 <= model_idx < len(model):
                target = row["model"][model_idx]
                entry.update(model_bottom_z=target["z"] - target["height"],
                             gap_mm=target["z"] - target["height"] - layer["z"],
                             xy_overlap_mm2=polygon_area(layer["polygons"]).intersection(model[model_idx][1]).area)
            top_contacts.append(entry)
        contacts = []
        for layer in row["groups"]["bottom"]:
            bottom = layer["z"] - layer["height"]
            for part in components(polygon_area(layer["polygons"])):
                below = [(z, part.intersection(area).area) for z, area in model if z <= bottom + 1e-6]
                below = [(z, overlap) for z, overlap in below if overlap > 1e-6]
                z, overlap = max(below, default=(None, 0.))
                contacts.append({"interface_z": layer["z"], "extrusion_bottom_z": bottom,
                                 "model_top_z": z, "xy_overlap_mm2": overlap,
                                 "gap_mm": bottom - z if z is not None else None,
                                 "bounds": part.bounds})
        areas = {}
        for group, layers in row["groups"].items():
            by_z = defaultdict(list)
            for layer in layers:
                by_z[layer["z"]].append(polygon_area(layer["polygons"]))
            areas[group] = [{"z": z, "components": len(components(unary_union(parts))),
                             "area_mm2": unary_union(parts).area}
                            for z, parts in sorted(by_z.items())]
        result["objects"].append({"contact_measurement": "generator footprint above XY-overlapping printed model layer",
                                  "bottom_contacts": contacts, "top_contacts": top_contacts, "source_areas": areas})
    return result


def analyze(path):
    paths, planes, errors = read_paths(path)
    grouped = defaultdict(list)
    for p in paths:
        grouped[p["z"]].append(p)
    report = {"gcode": str(path), "sha256": digest(path), "layers": len(planes), "z_values": planes,
              "role_paths": dict(Counter(p["role"] for p in paths if p["role"] in ROLES)),
              "error_markers": errors, "layer_reports": []}
    for z in planes:
        layer_paths = grouped[z]
        outlines = []
        for p in layer_paths:
            if p["role"] == "Support" and math.dist(p["points"][0], p["points"][-1]) <= 0.01:
                poly = Polygon(p["points"]).buffer(0)
                if poly.area > 0.01:
                    outlines.append(poly)
        bodies = components(unary_union(outlines))
        centers = [tuple(p.centroid.coords)[0] for p in bodies]
        report["layer_reports"].append({"z": z, "closed_trunk_components": len(bodies),
            "closed_trunk_centers": centers, "closed_trunk_spacing": spacing(centers),
            "roles": {role: {"paths": sum(p["role"] == role for p in layer_paths),
                "length_mm": sum(sum(p["segments"]) for p in layer_paths if p["role"] == role),
                "short_segments_below_0_3mm": sum(s < 0.3 for p in layer_paths if p["role"] == role for s in p["segments"])}
                for role in ROLES}})
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", type=Path)
    parser.add_argument("gcode", type=Path)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--stage-trace", type=Path)
    parser.add_argument("--log", type=Path, action="append", default=[])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = analyze(args.gcode)
    with zipfile.ZipFile(args.project) as archive:
        settings = json.loads(archive.read("Metadata/project_settings.config"))
    report.update(project=str(args.project), project_sha256=digest(args.project),
                  configured_bottom_gap_mm=settings.get("support_bottom_z_distance"),
                  contact_geometry=geometry_evidence(args.stage_trace) if args.stage_trace else None,
                  limitations=["Closed outlines do not reveal centers of trunks already joined by polygon union.",
                               "Generator contact gaps use printed model slice surfaces, not nominal triangle-mesh surfaces.",
                               "Spacing CV describes this slice; it does not certify uniformity on arbitrary geometry."])
    report["logs"] = [{"path": str(p), "sha256": digest(p),
                       "messages": [s for s in p.read_text(encoding="utf-8", errors="replace").splitlines()
                                    if re.search(r"\b(?:warning|error|exception)\b", s, re.I)]}
                      for p in args.log]
    if args.baseline:
        baseline = analyze(args.baseline)
        report["baseline"] = {k: baseline[k] for k in ("gcode", "sha256", "layers", "role_paths")}
        report["baseline"]["path_ratio"] = {role: report["role_paths"].get(role, 0) / count
                                               for role, count in baseline["role_paths"].items() if count}
    payload = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.write_text(payload, encoding="utf-8")
    else:
        print(payload)
    return 2 if report["error_markers"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
