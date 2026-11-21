#!/usr/bin/env python3
"""Compare ported rear-main triplets to Samsung source, without touching hardware."""
import argparse
import importlib.util
import json
import re
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('samsung', type=Path, help='is-cis-2ld-setA.h')
    parser.add_argument('--port', type=Path, default=Path(__file__).resolve().parents[2] /
                        'xnu/files/bsd/dev/redwood_camera_s20_2ld.h')
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('import_2ld', Path(__file__).with_name('import-2ld-setfile.py'))
    importer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(importer)
    source = args.samsung.read_text()
    port = args.port.read_text()
    report = []
    for stock_name, port_name in importer.TABLES:
        expected = importer.table(source, stock_name)
        match = re.search(r'\b' + re.escape(port_name) + r'\[\]\s*=\s*\{(.*?)\};', port, re.S)
        if not match:
            parser.exit(1, f'missing port table: {port_name}\n')
        body = re.sub(r'/\*.*?\*/|//[^\n]*', '', match[1], flags=re.S)
        actual = [int(t.strip().rstrip('uUlL'), 0) for t in body.split(',') if t.strip()]
        registers, page = {}, None
        for reg, value, width in zip(expected[::3], expected[1::3], expected[2::3]):
            if reg == 0xfcfc:
                page = value
            elif page == 0x4000 and reg < 0x10000:
                registers[f'{reg:#06x}'] = dict(value=hex(value), width=width)
        selected = {key: registers[key] for key in ('0x0340', '0x0342', '0x034c', '0x034e',
                    '0x0900', '0x021e', '0x0b30', '0x0b04', '0x0114') if key in registers}
        first_difference = next((i for i, (a, b) in enumerate(zip(expected, actual)) if a != b), None)
        report.append(dict(table=port_name, identical=expected == actual,
                           stock_triplets=len(expected)//3, port_words=len(actual),
                           first_differing_word=first_difference, mode_registers=selected))
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if all(r['identical'] for r in report) else 1)


if __name__ == '__main__':
    main()
