#!/usr/bin/env python3
"""Decode selected FINGE QDBL diagnostics; never print payloads or template IDs.

Evidence: TA 0x15d124 (headers), 0x15e9f0 (sizes), 0x15d644 (metric offsets).
Metric names below are opaque identifiers, not inferred failure explanations.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import struct

SIZES = {1: 42, 2: 46, 3: 46, 4: 28, 5: 73, 6: 28, 7: 69,
         8: 53, 10: 19, 11: 24, 12: 33, 13: 54, 14: 21}


def summarize(blob):
    if len(blob) > 0x8000:
        raise ValueError('QDBL export exceeds this TA ring capacity')
    offset = 0
    counts, verify, outcomes = Counter(), [], []
    while offset < len(blob):
        if len(blob) - offset < 14:
            raise ValueError('Truncated diagnostic header')
        version, kind, length = struct.unpack_from('<BBH', blob, offset)
        if version != 0x28 or SIZES.get(kind) != length or offset + length > len(blob):
            raise ValueError('Unknown or truncated diagnostic record')
        record = blob[offset:offset + length]
        counts[kind] += 1
        if kind == 7:
            verify.append(dict(metric_b3=record[0x12] >> 4,
                               metric_b4=record[0x12] & 15, metric_b5=record[0x13],
                               metric_b6=struct.unpack_from("<H", record, 0x14)[0],
                               metric_b7=struct.unpack_from("<H", record, 0x16)[0]))
        elif kind == 11:
            outcomes.append(dict(matched_flag=record[0x12] >> 4,
                                 metric_cb=record[0x12] & 15, aed=record[0x13] & 1))
        offset += length
    return dict(bytes=len(blob), record_counts=dict(sorted(counts.items())),
                verify_metrics=verify, identify_outcomes=outcomes)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(summarize(args.input.read_bytes()), indent=2))
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + '\n')
