#!/usr/bin/env python3
"""Offline camera wiring inventory. Never opens or writes a device.

Inputs are trusted local FDT captures. Output contains hardware metadata only.
External overlay fixups remain symbolic; 0xffffffff is never a physical address.
"""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct

NODES = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'soc-args.py'))['nodes_of']
STRINGS = {'compatible', 'status', 'sensor_maker', 'sensor_name', 'setfile_name',
           'setfile', 'pname', 'pinctrl-names', 'reg-names', 'phy-names', 'samsung,pins', 'regulator-name'}
ALLOWED = STRINGS | {'reg', 'id', 'sensor_id', 'position', 'active_width', 'active_height',
    'bitwidth', 'max_framerate', 'mclk_ch', 'sensor_i2c_ch', 'common', 'vc0', 'vc1',
    'vc2', 'vc3', 'gpio_mclk', 'gpio', 'pin', 'share', 'sensor_scenario',
    'gpio_scenario', 'use_cphy', 'csi_ch', 'dma_ch', 'vc_ch', 'isolation',
    'reset', 'owner', 'samsung,pmu-syscon', 'samsung,reset-sysreg', 'clock-frequency', 'sda-gpios', 'scl-gpios', 'phys', 'mux_reg',
    'samsung,pin-function', 'gpios', 'regulator-min-microvolt',
    'regulator-max-microvolt', 'pinctrl-0', 'pinctrl-1', 'pinctrl-2', 'pinctrl-3'}


def strings(value):
    return value.rstrip(b'\0').decode('utf-8').split('\0')


def fixups(base, overlay):
    result = {}
    symbols = base.get('/__symbols__', {})
    for label, locations in overlay.get('/__fixups__', {}).items():
        target = strings(symbols[label])[0] if label in symbols else None
        for location in strings(locations):
            path, prop, offset = location.rsplit(':', 2)
            offset = int(offset)
            raw = overlay.get(path, {}).get(prop)
            if raw is None or offset < 0 or offset % 4 or offset + 4 > len(raw):
                raise ValueError('invalid fixup location: ' + location)
            key = (path, prop, offset // 4)
            if key in result:
                raise ValueError('duplicate fixup: ' + location)
            result[key] = {'symbol': label, 'base_path': target}
    return result


def inventory(base_blob, overlay_blob):
    base, overlay = NODES(base_blob), NODES(overlay_blob)
    refs = fixups(base, overlay)
    roots = [p for p in overlay if p.rsplit('/', 1)[-1].lower().startswith(
        ('is_sensor_2la@', 'is_sensor_gw2@', 'is-2la@', 'is-gw2@'))]
    selected = {p for p in overlay if any(p == r or p.startswith(r + '/') for r in roots)}
    # Include each CIS's bus fragment: target resolves through external fixups.
    for root in roots:
        selected.add('/' + root.split('/')[1])
        selected.add(root.rsplit('/', 1)[0])
    # Board overlays override base CSIS/WDMA routes and PHY mode. Base-only
    # inventories can silently choose the wrong DMA engine and D-/C-PHY mode.
    for label in ('is_sensor2', 'is_sensor4'):
        for location in strings(overlay.get('/__fixups__', {}).get(label, b'')):
            if not location:
                continue
            fragment = location.rsplit(':', 2)[0]
            selected.update(p for p in overlay if p == fragment or p.startswith(fragment + '/'))
    supplies = {strings(overlay[p]['pname'])[0] for p in selected
                if 'pname' in overlay[p]}
    selected.update(p for p, v in overlay.items()
                    if 'regulator-name' in v and strings(v['regulator-name'])[0] in supplies)
    referenced = {v['base_path'] for (p, _, _), v in refs.items()
                  if p in selected and v['base_path']}
    # Framework routes are separate from I2C sensor nodes.
    referenced.update(p for p in base if p.startswith(('/is_sensor2@', '/is_sensor4@')))

    def properties(path, props, overlay_node):
        out = {}
        for name, raw in props.items():
            if name not in ALLOWED and name != 'target':
                continue
            if name in STRINGS:
                out[name] = strings(raw)
            else:
                if len(raw) % 4:
                    raise ValueError('unaligned cells: ' + path + ':' + name)
                cells = list(struct.unpack('>' + 'I' * (len(raw)//4), raw))
                for i, value in enumerate(cells):
                    ref = refs.get((path, name, i)) if overlay_node else None
                    if ref:
                        cells[i] = ref
                    elif value == 0xffffffff:
                        cells[i] = {'unresolved_cell': '0xffffffff'}
                out[name] = cells
        return out

    return {'schema': 1, 'base_sha256': hashlib.sha256(base_blob).hexdigest(),
        'overlay_sha256': hashlib.sha256(overlay_blob).hexdigest(),
        'status': 'offline inventory; no sensor identity or capture verified',
        'overlay_nodes': {p: properties(p, overlay[p], True) for p in sorted(selected)},
        'base_nodes': {p: properties(p, base[p], False) for p in sorted(referenced)}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base', type=Path)
    parser.add_argument('overlays', type=Path, nargs='+')
    args = parser.parse_args()
    print(json.dumps([inventory(args.base.read_bytes(), p.read_bytes())
                      for p in args.overlays], indent=2, sort_keys=True))


if __name__ == '__main__':
    main()
