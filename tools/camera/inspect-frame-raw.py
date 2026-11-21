#!/usr/bin/env python3
"""Inspect a complete RWFRAME1 RAW10 dump without image libraries.

Spatial statistics are scene-dependent. Use an opaque covered lens to measure
pedestal; never treat ordinary scene statistics as a dark calibration.
"""
import argparse
import json
import math
from pathlib import Path
import struct


def inspect(data, rows=16, columns=8):
    if len(data) < 120:
        raise ValueError('incomplete frame header')
    magic, header, width, height, stride, fmt, bayer, black = struct.unpack_from('<8s7I', data)
    if magic != b'RWFRAME1' or header != 120 or fmt != 1:
        raise ValueError('expected current RWFRAME1 RAW10 ABI')
    if not width or width % 4 or height % 2 or not height or stride < width // 4 * 5:
        raise ValueError('invalid dimensions/stride')
    if len(data) != header + stride * height:
        raise ValueError('incomplete or extra frame bytes')
    sequence, ticks = struct.unpack_from('<2Q', data, 40)
    controls = struct.unpack_from('<12I', data, 56)
    tiles = []
    for by in range(rows):
        line = []
        for bx in range(columns):
            samples = [[] for _ in range(4)]
            y0, y1 = (height * by // rows) & ~1, (height * (by + 1) // rows) & ~1
            x0, x1 = (width * bx // columns) & ~3, (width * (bx + 1) // columns) & ~3
            for y in range(y0, y1, 4):
                for x in range(x0, x1, 8):
                    for dy in range(2):
                        at = header + (y + dy) * stride + x // 4 * 5
                        g = data[at:at + 5]
                        for dx in range(2):
                            samples[dy * 2 + dx].append(g[dx] * 4 + ((g[4] >> (dx * 2)) & 3))
            stats = []
            for values in samples:
                if not values:
                    raise ValueError('grid too fine for frame')
                values.sort()
                mean = sum(values) / len(values)
                stats.append(dict(n=len(values), mean=mean,
                                  std=math.sqrt(sum((v-mean)**2 for v in values)/len(values)),
                                  p01=values[(len(values)-1)//100], median=values[len(values)//2],
                                  p99=values[(len(values)-1)*99//100]))
            line.append(stats)
        tiles.append(line)
    return dict(width=width, height=height, stride=stride, bayer=bayer, header_black=black,
                sequence=sequence, ticks=ticks, exposure_lines=controls[0], line_ns=controls[1],
                gain=controls[2], gain_unity=controls[3], sensor=controls[9],
                site_order='top-left, top-right, bottom-left, bottom-right',
                warning='Scene-dependent statistics; not automatically a calibration.', tiles=tiles)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('raw', type=Path)
    p.add_argument('--output', type=Path)
    args = p.parse_args()
    try:
        report = inspect(args.raw.read_bytes())
    except ValueError as e:
        p.error(str(e))
    text = json.dumps(report, indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end='')


if __name__ == '__main__':
    main()
