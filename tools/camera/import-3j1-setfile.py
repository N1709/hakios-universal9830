#!/usr/bin/env python3
"""Extract the SAK3J1 global table and conservative 1824x1368 mode from Samsung's setA header."""
import argparse
import re
from pathlib import Path

OPS = {
    "I2C_MODE_BURST_ADDR": 0xF0000001,
    "I2C_MODE_BURST_DATA": 0xF0000002,
    "I2C_MODE_DELAY": 0xF0000003,
}
TABLES = (
    ("sensor_3j1_setfile_A_Global", "redwood_3j1_global"),
    ("sensor_3j1_setfile_A_1824x1368_30fps_nonPD", "redwood_3j1_1824x1368_30fps"),
    ("sensor_3j1_setfile_A_1824x1368_30fps", "redwood_3j1_1824x1368_30fps_pd"),
    ("sensor_3j1_setfile_A_3648x2736_30fps", "redwood_3j1_3648x2736_30fps_pd"),
)


def table(source: str, name: str) -> list[int]:
    clean = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
    match = re.search(r"const\s+u32\s+" + re.escape(name) + r"\[\]\s*=\s*\{(.*?)\};", clean, re.S)
    if match is None:
        raise ValueError(f"table not found: {name}")
    values = []
    for token in match.group(1).split(","):
        token = token.strip()
        if not token:
            continue
        values.append(OPS[token] if token in OPS else int(token, 0))
    if not values or len(values) % 3:
        raise ValueError(f"malformed triplets in {name}: {len(values)} values")
    return values


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Samsung is-cis-3j1-setA.h")
    parser.add_argument("output", type=Path, help="output XNU header")
    args = parser.parse_args()
    source = args.source.read_text()
    lines = [
        "/*",
        " * SAK3J1 S5K3J1S setA tables, extracted from Samsung Exynos990 is-cis-3j1-setA.h.",
        " * Global initialization plus the 1824x1368 30 fps non-PDAF mode. Triplet semantics",
        " * and F0000001..3 operation tags follow Samsung's is-cis.c / is-helper-i2c.h.",
        " */",
        "#ifndef REDWOOD_CAMERA_S20_3J1_H",
        "#define REDWOOD_CAMERA_S20_3J1_H",
        "#include <stdint.h>",
    ]
    for input_name, output_name in TABLES:
        values = table(source, input_name)
        lines.append(f"static const uint32_t {output_name}[] = {{")
        for i in range(0, len(values), 9):
            lines.append("\t" + ", ".join(f"0x{x:08x}U" for x in values[i:i + 9]) + ",")
        lines.append("};")
        lines.append(f"#define {output_name.upper()}_WORDS (sizeof({output_name}) / sizeof({output_name}[0]))")
        lines.append("")
    lines.append("#endif")
    args.output.write_text("\n".join(lines) + "\n")
    print(f"wrote {args.output}: global={len(table(source, TABLES[0][0])) // 3} triplets, mode={len(table(source, TABLES[1][0])) // 3} triplets")


if __name__ == "__main__":
    main()
