#!/usr/bin/env python3
"""Summarize object-simulation process changes in exported G-code.

The script is intentionally read-only. It reports per-layer/per-role speed and
acceleration ranges so process tuning can be checked without adding diagnostic
comments to exported G-code.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import statistics
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path


ROLE_PREFIXES = (
    ";TYPE:",
    "; FEATURE:",
    ";FEATURE:",
)


@dataclass
class MoveSample:
    speed: float | None
    acceleration: float | None
    fan: float | None
    length: float


@dataclass
class RoleStats:
    speeds: list[float] = field(default_factory=list)
    accelerations: list[float] = field(default_factory=list)
    fan_values: list[float] = field(default_factory=list)
    samples: list[MoveSample] = field(default_factory=list)
    extrusion_mm: float = 0.0

    def add(self, speed: float | None, accel: float | None, fan: float | None, length: float) -> None:
        self.samples.append(MoveSample(speed, accel, fan, max(0.0, length)))
        if speed is not None:
            self.speeds.append(speed)
        if accel is not None:
            self.accelerations.append(accel)
        if fan is not None:
            self.fan_values.append(fan)
        self.extrusion_mm += max(0.0, length)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Analyze speed and acceleration variation in exported G-code.")
    parser.add_argument("gcode", type=Path, help="Path to a G-code file.")
    parser.add_argument("--top", type=int, default=24, help="Number of layer/role rows to print.")
    parser.add_argument("--min-count", type=int, default=20, help="Minimum extrusion moves for a row.")
    parser.add_argument("--json", action="store_true", help="Emit machine-readable JSON.")
    return parser.parse_args()


def strip_comment(line: str) -> tuple[str, str]:
    if ";" not in line:
        return line.strip(), ""
    code, comment = line.split(";", 1)
    return code.strip(), ";" + comment.strip()


def parse_role(comment: str, current: str) -> str:
    for prefix in ROLE_PREFIXES:
        if comment.startswith(prefix):
            return comment[len(prefix):].strip() or current
    return current


def parse_layer(comment: str, current: int) -> int:
    if comment.startswith(";LAYER_CHANGE"):
        return current + 1
    match = re.match(r";LAYER:\s*(-?\d+)", comment)
    if match:
        return max(0, int(match.group(1)))
    return current


def parse_words(code: str) -> dict[str, float]:
    words: dict[str, float] = {}
    for token in code.split():
        if len(token) < 2:
            continue
        key = token[0].upper()
        if key < "A" or key > "Z":
            continue
        try:
            words[key] = float(token[1:])
        except ValueError:
            continue
    return words


def parse_klipper_accel(code: str) -> float | None:
    if not code.upper().startswith("SET_VELOCITY_LIMIT"):
        return None
    match = re.search(r"\bACCEL\s*=\s*([0-9.]+)", code, re.IGNORECASE)
    return float(match.group(1)) if match else None


def segment_length(words: dict[str, float], x: float | None, y: float | None) -> float:
    nx = words.get("X", x)
    ny = words.get("Y", y)
    if x is None or y is None or nx is None or ny is None:
        return 0.0
    return math.hypot(nx - x, ny - y)


def is_extrusion(words: dict[str, float], current_e: float, relative_e: bool) -> tuple[bool, float]:
    if "E" not in words:
        return False, current_e
    e = words["E"]
    delta = e if relative_e else e - current_e
    next_e = current_e + e if relative_e else e
    return delta > 1e-6, next_e


def fmt_range(values: list[float], unit: str) -> str:
    if not values:
        return "-"
    low = min(values)
    high = max(values)
    avg = statistics.fmean(values)
    spread = high - low
    return f"{avg:.3f} {unit} avg, {low:.3f}-{high:.3f}, range {spread:.3f}"


def percentile(values: list[float], p: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    pos = (len(ordered) - 1) * max(0.0, min(1.0, p))
    lo = math.floor(pos)
    hi = math.ceil(pos)
    if lo == hi:
        return ordered[lo]
    frac = pos - lo
    return ordered[lo] * (1.0 - frac) + ordered[hi] * frac


def adjacent_deltas(values: list[float | None]) -> list[float]:
    deltas: list[float] = []
    previous: float | None = None
    for value in values:
        if value is None:
            continue
        if previous is not None:
            deltas.append(abs(value - previous))
        previous = value
    return deltas


def role_is_model_body(role: str) -> bool:
    lower = role.lower()
    excluded = (
        "support",
        "bridge",
        "skirt",
        "brim",
        "wipe",
        "travel",
        "custom",
        "overhang",
    )
    return not any(token in lower for token in excluded)


def role_is_solid_or_fill(role: str) -> bool:
    lower = role.lower()
    included = (
        "bottom",
        "top",
        "solid",
        "infill",
        "gap",
        "fill",
    )
    return role_is_model_body(role) and any(token in lower for token in included)


def metric_range(values: list[float]) -> float:
    return max(values) - min(values) if values else 0.0


def mean(values: list[float]) -> float | None:
    return statistics.fmean(values) if values else None


def row_metrics(layer: int, role: str, data: RoleStats) -> dict[str, object]:
    speed_values = [sample.speed for sample in data.samples]
    accel_values = [sample.acceleration for sample in data.samples]
    speed_deltas = adjacent_deltas(speed_values)
    accel_deltas = adjacent_deltas(accel_values)
    speed_range = metric_range(data.speeds)
    accel_range = metric_range(data.accelerations)
    speed_p95_delta = percentile(speed_deltas, 0.95)
    accel_p95_delta = percentile(accel_deltas, 0.95)

    flags: list[str] = []
    score = 100.0
    model_body = role_is_model_body(role)
    solid_or_fill = role_is_solid_or_fill(role)

    if not data.speeds:
        flags.append("NO_SPEED_DATA")
        score -= 30.0
    elif model_body and solid_or_fill and len(data.speeds) >= 50 and speed_range < 0.05:
        flags.append("FLAT_SPEED")
        score -= 22.0

    if not data.accelerations:
        flags.append("NO_ACCEL_DATA")
        score -= 14.0
    elif model_body and len(data.accelerations) >= 50 and accel_range < 40.0:
        flags.append("FLAT_ACCEL")
        score -= 30.0 if solid_or_fill else 20.0

    if speed_p95_delta is not None:
        if speed_p95_delta > 0.35:
            flags.append("SPEED_JUMP")
            score -= min(30.0, 12.0 + (speed_p95_delta - 0.35) * 30.0)
        elif speed_p95_delta > 0.12:
            flags.append("SPEED_ROUGH")
            score -= min(14.0, (speed_p95_delta - 0.12) * 35.0)

    if accel_p95_delta is not None:
        if accel_p95_delta > 500.0:
            flags.append("ACCEL_JUMP")
            score -= min(30.0, 10.0 + (accel_p95_delta - 500.0) / 80.0)
        elif accel_p95_delta > 200.0:
            flags.append("ACCEL_ROUGH")
            score -= min(12.0, (accel_p95_delta - 200.0) / 35.0)

    score = max(0.0, min(100.0, score))
    return {
        "layer": layer,
        "role": role,
        "moves": len(data.samples),
        "extrusion_mm": data.extrusion_mm,
        "model_body": model_body,
        "speed_avg": mean(data.speeds),
        "speed_min": min(data.speeds) if data.speeds else None,
        "speed_max": max(data.speeds) if data.speeds else None,
        "speed_range": speed_range,
        "speed_p95_adjacent_delta": speed_p95_delta,
        "accel_avg": mean(data.accelerations),
        "accel_min": min(data.accelerations) if data.accelerations else None,
        "accel_max": max(data.accelerations) if data.accelerations else None,
        "accel_range": accel_range,
        "accel_p95_adjacent_delta": accel_p95_delta,
        "fan_avg": mean(data.fan_values),
        "fan_min": min(data.fan_values) if data.fan_values else None,
        "fan_max": max(data.fan_values) if data.fan_values else None,
        "score": score,
        "flags": flags,
    }


def layer_recovery_flags(metrics: list[dict[str, object]]) -> list[dict[str, object]]:
    by_role: dict[str, list[dict[str, object]]] = defaultdict(list)
    for item in metrics:
        if not item["model_body"]:
            continue
        by_role[str(item["role"])].append(item)

    findings: list[dict[str, object]] = []
    for role, items in by_role.items():
        items.sort(key=lambda item: int(item["layer"]))
        previous_speed: float | None = None
        previous_accel: float | None = None
        for item in items:
            speed = item["speed_avg"]
            accel = item["accel_avg"]
            layer = int(item["layer"])
            if isinstance(speed, float) and previous_speed is not None and speed + 0.25 < previous_speed:
                findings.append({
                    "layer": layer,
                    "role": role,
                    "type": "SPEED_RECOVERY_REVERSAL",
                    "previous": previous_speed,
                    "current": speed,
                })
            if isinstance(accel, float) and previous_accel is not None and accel + 250.0 < previous_accel:
                findings.append({
                    "layer": layer,
                    "role": role,
                    "type": "ACCEL_RECOVERY_REVERSAL",
                    "previous": previous_accel,
                    "current": accel,
                })
            if isinstance(speed, float):
                previous_speed = speed
            if isinstance(accel, float):
                previous_accel = accel
    return findings


def aggregate_score(metrics: list[dict[str, object]], recovery_findings: list[dict[str, object]]) -> float:
    weighted = 0.0
    weight_sum = 0.0
    for item in metrics:
        weight = max(1.0, float(item["extrusion_mm"]))
        if not item["model_body"]:
            weight *= 0.25
        weighted += float(item["score"]) * weight
        weight_sum += weight
    score = weighted / weight_sum if weight_sum > 0.0 else 0.0
    critical_penalty = 0.0
    for item in metrics:
        if not item["model_body"]:
            continue
        flags = set(item["flags"])
        layer = int(item["layer"])
        role = str(item["role"])
        solid_or_fill = role_is_solid_or_fill(role)
        if "FLAT_ACCEL" in flags:
            critical_penalty += 1.5 if solid_or_fill and layer <= 3 else 0.8
        if "FLAT_SPEED" in flags:
            critical_penalty += 1.2 if solid_or_fill and layer <= 3 else 0.6
        if "SPEED_JUMP" in flags or "ACCEL_JUMP" in flags:
            critical_penalty += 1.4
        elif "SPEED_ROUGH" in flags or "ACCEL_ROUGH" in flags:
            critical_penalty += 0.4
    score -= min(20.0, critical_penalty)
    score -= min(12.0, len(recovery_findings) * 1.5)
    return max(0.0, min(100.0, score))


def fmt_optional(value: object, digits: int = 3) -> str:
    if isinstance(value, (float, int)):
        return f"{float(value):.{digits}f}"
    return "-"


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    args = parse_args()
    if not args.gcode.exists():
        raise SystemExit(f"File not found: {args.gcode}")

    stats: dict[tuple[int, str], RoleStats] = defaultdict(RoleStats)
    current_layer = 0
    current_role = "unknown"
    current_speed: float | None = None
    current_accel: float | None = None
    current_fan: float | None = None
    x: float | None = None
    y: float | None = None
    current_e = 0.0
    relative_e = False

    with args.gcode.open("r", encoding="utf-8", errors="replace") as handle:
        for raw_line in handle:
            code, comment = strip_comment(raw_line)
            if comment:
                current_layer = parse_layer(comment, current_layer)
                current_role = parse_role(comment, current_role)
            if not code:
                continue

            upper = code.upper()
            if upper.startswith("M82"):
                relative_e = False
            elif upper.startswith("M83"):
                relative_e = True
            elif upper.startswith("G92"):
                words = parse_words(code)
                if "E" in words:
                    current_e = words["E"]
            elif upper.startswith("M106"):
                words = parse_words(code)
                if "S" in words:
                    current_fan = max(0.0, min(100.0, words["S"] * 100.0 / 255.0))
            elif upper.startswith("M204"):
                words = parse_words(code)
                for key in ("S", "P"):
                    if key in words:
                        current_accel = words[key]
                        break
            else:
                klipper_accel = parse_klipper_accel(code)
                if klipper_accel is not None:
                    current_accel = klipper_accel

            if not (upper.startswith("G0") or upper.startswith("G1")):
                continue

            words = parse_words(code)
            if "F" in words:
                current_speed = words["F"] / 60.0
            length = segment_length(words, x, y)
            extruding, next_e = is_extrusion(words, current_e, relative_e)
            if "X" in words:
                x = words["X"]
            if "Y" in words:
                y = words["Y"]
            current_e = next_e

            if extruding:
                stats[(current_layer, current_role)].add(current_speed, current_accel, current_fan, length)

    rows = [
        (layer, role, data)
        for (layer, role), data in stats.items()
        if len(data.speeds) >= args.min_count
    ]
    rows.sort(key=lambda item: (item[0], item[1]))
    metrics = [row_metrics(layer, role, data) for layer, role, data in rows]
    recovery_findings = layer_recovery_flags(metrics)
    overall_score = aggregate_score(metrics, recovery_findings)

    if args.json:
        print(json.dumps({
            "gcode": str(args.gcode),
            "rows": metrics,
            "recovery_findings": recovery_findings,
            "score": overall_score,
        }, ensure_ascii=False, indent=2))
        return 0

    print(f"G-code: {args.gcode}")
    print(f"Rows: {len(rows)} with at least {args.min_count} extrusion moves")
    print(f"Process smoothness score: {overall_score:.1f} / 100")
    if recovery_findings:
        print(f"Layer recovery warnings: {len(recovery_findings)}")
    else:
        print("Layer recovery warnings: 0")
    print()
    print("Layer | Role | Moves | Length mm | Speed | Acceleration | Adjacent p95 | Flags")
    print("-" * 150)
    for item, (layer, role, data) in zip(metrics[:args.top], rows[:args.top]):
        adjacent = (
            f"dV {fmt_optional(item['speed_p95_adjacent_delta'])} mm/s, "
            f"dA {fmt_optional(item['accel_p95_adjacent_delta'], 1)} mm/s^2"
        )
        flags = ",".join(item["flags"]) if item["flags"] else "OK"
        print(
            f"{layer:5d} | {role[:30]:30s} | {len(data.speeds):5d} | "
            f"{data.extrusion_mm:9.1f} | {fmt_range(data.speeds, 'mm/s'):32s} | "
            f"{fmt_range(data.accelerations, 'mm/s^2'):36s} | {adjacent:30s} | {flags}"
        )

    if recovery_findings:
        print()
        print("First recovery warnings:")
        for finding in recovery_findings[:10]:
            print(
                f"  layer {finding['layer']}, {finding['role']}: {finding['type']} "
                f"{finding['previous']:.3f} -> {finding['current']:.3f}"
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
