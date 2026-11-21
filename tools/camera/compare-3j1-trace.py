#!/usr/bin/env python3
"""Compare Samsung's i2c_write trace with the SAK3J1 global and PDAF tables.

Usage: compare-3j1-trace.py REFERENCE_LOG
Accepts the saved text report or its NUL-padded raw log. Trace payloads may
be truncated: report that limitation rather than treating unseen bytes as equal.
"""
import argparse
import difflib
import json
from pathlib import Path
import re


def table_transactions(source, name):
    body = re.search(re.escape(name) + r"\[\] = \{(.*?)\};", source, re.S)
    if not body:
        raise ValueError(f"missing table {name}")
    words = [int(x, 16) for x in re.findall(r"0x([0-9a-f]+)U", body[1])]
    result, burst = [], None
    for offset in range(0, len(words), 3):
        reg, value, width = words[offset:offset + 3]
        if reg == 0xf0000002:
            if burst is None:
                raise ValueError("burst data without burst address")
            burst += value.to_bytes(2, "big")
            continue
        if burst is not None:
            result.append(bytes(burst))
            burst = None
        if reg == 0xf0000001:
            burst = bytearray(value.to_bytes(2, "big"))
        elif reg != 0xf0000003:
            if width != 2:
                raise ValueError("unexpected setfile register width")
            result.append(reg.to_bytes(2, "big") + value.to_bytes(2, "big"))
    if burst is not None:
        result.append(bytes(burst))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    args = parser.parse_args()
    text = args.reference.read_bytes().replace(b"\0", b"").decode(errors="replace")
    marker = "=== sensor I2C write trace\n"
    if marker not in text:
        parser.error("no saved sensor I2C trace section")
    events = []
    for match in re.finditer(r"([0-9]+\.[0-9]+): i2c_write: .*?a=010 .*?l=(\d+) \[([0-9a-f-]+)\]",
                             text.split(marker, 1)[1]):
        length = int(match[2])
        data = bytes.fromhex(match[3].replace("-", ""))
        if length > 2:  # exclude address-only transactions used by reads
            events.append((length, data))
            if data == b"\x01\x00\x01":
                events.pop()
                break
    else:
        parser.error("no stream-on write; trace may be incomplete")
    source = (Path(__file__).resolve().parents[2] /
              "xnu/files/bsd/dev/redwood_camera_s20_3j1.h").read_text()
    expected = table_transactions(source, "redwood_3j1_global")
    expected += table_transactions(source, "redwood_3j1_1824x1368_30fps_pd")
    # Linux's trace formatter shows at most 64 payload bytes in this capture.
    keys = [(len(data), data[:64]) for data in expected]
    changes = []
    matched = 0
    for kind, a, b, c, d in difflib.SequenceMatcher(a=keys, b=events, autojunk=False).get_opcodes():
        if kind == "equal":
            matched += b - a
        else:
            changes.append({"kind": kind, "table_index": a, "trace_index": c,
                            "table": [{"length": n, "bytes": x.hex()} for n, x in keys[a:b]],
                            "trace": [{"length": n, "bytes": x.hex()} for n, x in events[c:d]]})
    print(json.dumps({"table_transactions": len(keys), "pre_stream_transactions": len(events),
                      "matched_transactions": matched,
                      "truncated_payloads": sum(n > len(data) for n, data in events),
                      "changes": changes}, indent=2))
    return 0 if matched == len(keys) else 1


if __name__ == "__main__":
    raise SystemExit(main())
