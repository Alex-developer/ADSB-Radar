#!/usr/bin/env python3
"""Generate compact airport lookup tables for the ESP32 radar.

The source CSV is too expensive to parse on the device at start-up. This script
turns the fields the radar needs into C arrays that can be linked into the app:
latitude, longitude, ICAO code, and airport name. Coordinates are quantised to
microdegrees so the generated data stays compact and deterministic.
"""

import csv
import pathlib
import sys


def clean_code(value):
    """Return a four-character alphanumeric ICAO-style code."""
    code = "".join(ch for ch in value.strip().upper() if ("A" <= ch <= "Z") or ("0" <= ch <= "9"))
    return code[:4]


def clean_name(value):
    """Normalise whitespace and cap UTF-8 names to the generated field size."""
    text = " ".join(str(value or "").strip().split())
    return text.encode("utf-8")[:95].decode("utf-8", "ignore")


def c_string(value):
    """Escape a Python string as a C string literal."""
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    """Read the source CSV and write generated airport lookup files."""
    if len(sys.argv) != 4:
        print("usage: generate_airports.py airports.csv airport_data.cpp airport_data.h", file=sys.stderr)
        return 2

    csv_path = pathlib.Path(sys.argv[1])
    c_path = pathlib.Path(sys.argv[2])
    h_path = pathlib.Path(sys.argv[3])

    # Keep only rows that can be plotted. Bad data is skipped rather than
    # failing the build because public airport datasets occasionally contain
    # incomplete closed or historical records.
    records = []
    with csv_path.open("r", encoding="utf-8", newline="") as src:
        reader = csv.DictReader(src)
        for row in reader:
            try:
                lat = float(row["latitude_deg"])
                lon = float(row["longitude_deg"])
            except (KeyError, TypeError, ValueError):
                continue
            if not (-90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0):
                continue

            code = clean_code(row.get("icao_code", ""))
            name = clean_name(row.get("name", ""))
            records.append((round(lat * 1000000), round(lon * 1000000), code, name))

    h_path.parent.mkdir(parents=True, exist_ok=True)
    c_path.parent.mkdir(parents=True, exist_ok=True)

    h_path.write_text(
        "#pragma once\n"
        "\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n"
        "\n"
        "typedef struct {\n"
        "    int32_t lat_e6;\n"
        "    int32_t lon_e6;\n"
        "    char code[5];\n"
        "    char name[96];\n"
        "} airport_record_t;\n"
        "\n"
        "extern const airport_record_t airport_records[];\n"
        "extern const size_t airport_record_count;\n",
        encoding="utf-8",
    )

    lines = [
        '#include "airport_data.h"',
        "",
        "const airport_record_t airport_records[] = {",
    ]
    for lat_e6, lon_e6, code, name in records:
        lines.append(f"    {{ {lat_e6}, {lon_e6}, {c_string(code)}, {c_string(name)} }},")
    lines.extend(
        [
            "};",
            "",
            f"const size_t airport_record_count = {len(records)};",
            "",
        ]
    )
    c_path.write_text("\n".join(lines), encoding="utf-8")
    print(f"generated {len(records)} airport records")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
