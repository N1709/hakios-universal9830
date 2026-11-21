#!/usr/bin/env python3
"""Build a private, exact-readout S5K2LD pedestal map from covered-lens RAWs.

Usage: make-rear-dark-grid.py OUTPUT RAW [RAW ...]
Only one exposure/frame/CFA state is accepted; 2..8 measured gains, no extrapolation.
The capture operator must establish that the lens was opaque-covered. Scene RAWs
cannot be detected reliably and must never be used for this calibration.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

_spec = importlib.util.spec_from_file_location('raw_inspection', Path(__file__).with_name('inspect-frame-raw.py'))
raw = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(raw)


def build(paths):
    profiles = []
    metadata = None
    sources = []
    for path in paths:
        data = path.read_bytes()
        r = raw.inspect(data, rows=13, columns=17)
        controls = struct.unpack_from('<12I', data, 56)
        state = (r['width'], r['height'], r['bayer'], r['line_ns'], controls[5], r['exposure_lines'], r['gain_unity'])
        if state[:2] != (2016, 1512) or r['sensor'] != 1 or r['gain_unity'] != 32 or not 32 <= r['gain'] <= 512:
            raise ValueError('not supported rear binned RAW')
        if metadata is not None and state != metadata:
            raise ValueError('mixed readout/exposure states')
        metadata = state
        values = [r['tiles'][y][x][k]['mean'] for k in range(4) for y in range(13) for x in range(17)]
        if not all(0 <= v <= 256 for v in values):
            raise ValueError('pedestal out of supported range')
        profiles.append((r['gain'], values))
        sources.append(dict(file=path.name, sha256=hashlib.sha256(data).hexdigest(), gain=r['gain']))
    profiles.sort()
    if not 2 <= len(profiles) <= 8 or len(set(g for g, _ in profiles)) != len(profiles):
        raise ValueError('require 2..8 unique measured gains')
    w, h, bayer, line, frame, exposure, unity = metadata
    out = bytearray(struct.pack('<8s10I', b'RWBLK001', w, h, bayer, line, frame, exposure, len(profiles), 17, 13, unity))
    for gain, values in profiles:
        out += struct.pack('<I884f', gain, *values)
    return out, dict(sources=sources, readout=metadata, gains=[g for g, _ in profiles],
                     warning='Private per-module covered-lens calibration; exact exposure only, no stock tuning.')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('raw', nargs='+', type=Path)
    a = p.parse_args()
    try:
        data, report = build(a.raw)
    except ValueError as e:
        p.error(str(e))
    a.output.write_bytes(data)
    a.output.with_suffix('.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f'{len(data)} bytes; gains {report["gains"]}')


if __name__ == '__main__':
    main()
