#!/usr/bin/env python3
"""Generate simplified country boundary tables for the ESP32 radar.

The source GeoJSON is much larger than the device should parse at runtime. This
script extracts polygon rings, simplifies them with Ramer-Douglas-Peucker, and
writes C arrays containing quantised latitude/longitude points plus per-line
bounds. The bounds let the radar skip most lines before doing projection maths.
"""

import json
import math
import pathlib
import sys


# Degrees. This is deliberately coarse because the display is 720 x 720 and the
# boundaries are meant to be subtle context, not cartographic-grade outlines.
TOLERANCE_DEG = 0.03
COORD_SCALE = 100000


def sq_distance_to_segment(point, start, end):
    """Squared distance from a point to a line segment in lon/lat space."""
    px, py = point
    ax, ay = start
    bx, by = end
    dx = bx - ax
    dy = by - ay
    if dx == 0.0 and dy == 0.0:
        return (px - ax) * (px - ax) + (py - ay) * (py - ay)

    t = ((px - ax) * dx + (py - ay) * dy) / ((dx * dx) + (dy * dy))
    if t < 0.0:
        t = 0.0
    elif t > 1.0:
        t = 1.0

    x = ax + (t * dx)
    y = ay + (t * dy)
    return (px - x) * (px - x) + (py - y) * (py - y)


def simplify(points, tolerance):
    """Simplify one ring while preserving its endpoints."""
    if len(points) <= 2:
        return points

    keep = [False] * len(points)
    keep[0] = True
    keep[-1] = True
    stack = [(0, len(points) - 1)]
    tolerance_sq = tolerance * tolerance

    while stack:
        start, end = stack.pop()
        best_distance = -1.0
        best_index = -1
        for index in range(start + 1, end):
            distance = sq_distance_to_segment(points[index], points[start], points[end])
            if distance > best_distance:
                best_distance = distance
                best_index = index

        if best_distance > tolerance_sq and best_index > start:
            keep[best_index] = True
            stack.append((start, best_index))
            stack.append((best_index, end))

    return [point for point, should_keep in zip(points, keep) if should_keep]


def geometry_rings(geometry):
    """Yield each ring from a Polygon or MultiPolygon geometry."""
    if not geometry:
        return
    geometry_type = geometry.get("type")
    coordinates = geometry.get("coordinates") or []
    if geometry_type == "Polygon":
        for ring in coordinates:
            yield ring
    elif geometry_type == "MultiPolygon":
        for polygon in coordinates:
            for ring in polygon:
                yield ring


def quantize(value):
    """Quantise degrees into a signed fixed-point integer."""
    return int(round(float(value) * COORD_SCALE))


def c_string(value):
    """Escape a Python string as a C string literal."""
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    """Read GeoJSON and write simplified generated country boundary tables."""
    if len(sys.argv) != 4:
        print("usage: generate_boundaries.py world.geojson boundary_data.cpp boundary_data.h", file=sys.stderr)
        return 2

    geojson_path = pathlib.Path(sys.argv[1])
    c_path = pathlib.Path(sys.argv[2])
    h_path = pathlib.Path(sys.argv[3])

    with geojson_path.open("r", encoding="utf-8") as src:
        data = json.load(src)

    points = []
    lines = []

    for feature in data.get("features", []):
        properties = feature.get("properties") or {}
        name = str(properties.get("name") or "")[:31]
        for ring in geometry_rings(feature.get("geometry")):
            raw = []
            for coord in ring:
                if not isinstance(coord, list) or len(coord) < 2:
                    continue
                lon = float(coord[0])
                lat = float(coord[1])
                if -180.0 <= lon <= 180.0 and -90.0 <= lat <= 90.0:
                    raw.append((lon, lat))
            if len(raw) < 2:
                continue

            closed = len(raw) > 2 and raw[0] == raw[-1]
            work = raw[:-1] if closed else raw
            simplified = simplify(work, TOLERANCE_DEG)
            if closed and simplified and simplified[0] != simplified[-1]:
                simplified.append(simplified[0])
            if len(simplified) < 2:
                continue

            first = len(points)
            lat_values = []
            lon_values = []
            for lon, lat in simplified:
                lat_e5 = quantize(lat)
                lon_e5 = quantize(lon)
                points.append((lat_e5, lon_e5))
                lat_values.append(lat_e5)
                lon_values.append(lon_e5)

            lines.append((first, len(simplified), min(lat_values), max(lat_values),
                          min(lon_values), max(lon_values), name))

    h_path.parent.mkdir(parents=True, exist_ok=True)
    c_path.parent.mkdir(parents=True, exist_ok=True)

    h_path.write_text(
        "#pragma once\n"
        "\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n"
        "\n"
        "typedef struct {\n"
        "    int32_t lat_e5;\n"
        "    int32_t lon_e5;\n"
        "} boundary_point_t;\n"
        "\n"
        "typedef struct {\n"
        "    uint32_t first_point;\n"
        "    uint16_t point_count;\n"
        "    int32_t min_lat_e5;\n"
        "    int32_t max_lat_e5;\n"
        "    int32_t min_lon_e5;\n"
        "    int32_t max_lon_e5;\n"
        "    char name[32];\n"
        "} boundary_line_t;\n"
        "\n"
        "extern const boundary_point_t boundary_points[];\n"
        "extern const size_t boundary_point_count;\n"
        "extern const boundary_line_t boundary_lines[];\n"
        "extern const size_t boundary_line_count;\n",
        encoding="utf-8",
    )

    output = [
        '#include "country_boundary_data.h"',
        "",
        "const boundary_point_t boundary_points[] = {",
    ]
    for lat_e5, lon_e5 in points:
        output.append(f"    {{ {lat_e5}, {lon_e5} }},")
    output.extend([
        "};",
        "",
        f"const size_t boundary_point_count = {len(points)};",
        "",
        "const boundary_line_t boundary_lines[] = {",
    ])
    for first, count, min_lat, max_lat, min_lon, max_lon, name in lines:
        output.append(
            f"    {{ {first}, {count}, {min_lat}, {max_lat}, {min_lon}, {max_lon}, {c_string(name)} }},"
        )
    output.extend([
        "};",
        "",
        f"const size_t boundary_line_count = {len(lines)};",
        "",
    ])
    c_path.write_text("\n".join(output), encoding="utf-8")

    print(f"generated {len(lines)} boundary lines and {len(points)} points from {geojson_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
