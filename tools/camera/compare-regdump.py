#!/usr/bin/env python3
"""Compare the shared camera register list: Samsung reference (sec_debug SFR lines) vs XNU (cam.s20.front.stream bit 8).
usage: compare-regdump.py REFERENCE_MAIN_TXT XNU_STREAM_TXT [OUT_JSON]"""
import json, re, sys
from pathlib import Path
here = Path(__file__).resolve().parent
names = [l.split(None, 1) for l in (here.parent / 'reference-boot/camera-regdump-names.txt').read_text().splitlines()]
addrs = [int(a, 16) for a, _ in names]
ref_text = Path(sys.argv[1]).read_text(errors='replace')
# Samsung: "try to read 0xADDR" then "result : 0xVAL"; keep the reads after "CAMERA_REGDUMP begin" (last value per address).
ref = {}
pending = None
for line in ref_text.splitlines():
    m = re.search(r'simulate_SFR\(\) try to read (0x[0-9a-f]+)', line)
    if m: pending = int(m.group(1), 16); continue
    m = re.search(r'simulate_SFR\(\) result : (0x[0-9a-f]+)', line)
    if m and pending is not None: ref[pending] = int(m.group(1), 16); pending = None
xnu_line = next(l for l in Path(sys.argv[2]).read_text().splitlines() if l.startswith('regdump '))
xvals = [int(v, 16) for v in xnu_line.split(':', 1)[1].split()]
rows, diffs = [], []
for (a, n), x in zip(names, xvals):
    a = int(a, 16); s = ref.get(a)
    row = {'address': '%#010x' % a, 'name': n, 'samsung': None if s is None else '%#x' % s, 'xnu': '%#x' % x,
           'match': s == x}
    rows.append(row)
    if s is not None and s != x: diffs.append(row)
missing = [r['name'] for r in rows if r['samsung'] is None]
print(f'{len(rows)} registers, {len(rows) - len(missing)} read on Samsung, {len(diffs)} differ, {len(missing)} missing on Samsung')
for r in diffs: print(f"  {r['address']} {r['name']:<55} samsung {r['samsung']:<12} xnu {r['xnu']}")
if len(sys.argv) > 3: Path(sys.argv[3]).write_text(json.dumps({'differ': diffs, 'missing_on_samsung': missing, 'all': rows}, indent=1))
