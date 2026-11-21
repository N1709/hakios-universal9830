#!/usr/bin/env python3
"""Read-only fingerprint wiring inventory from locally owned DTBs.
Prints only selected hardware properties; never templates, keys, or firmware data.
DTBO fixups are reported as unresolved rather than guessed or used as addresses.
"""
import argparse
import json
from pathlib import Path
import runpy
import struct

PARSER = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'soc-args.py'))
KEYS = ('compatible', 'status', 'qcom,ipc-gpio', 'qcom,wuhb-gpio',
        'qcom,btp-regulator', 'qcom,modelinfo', 'qcom,position', 'pinctrl-names',
        'pinctrl-0', 'pinctrl-1', 'pinctrl-2')

def inventory(path):
    nodes = PARSER['nodes_of'](path.read_bytes())
    result = []
    for name, props in nodes.items():
        compat = props.get('compatible', b'').split(b'\0')
        if b'qcom,qbt2000' not in compat:
            continue
        selected = {}
        for key in KEYS:
            if key not in props:
                continue
            value = props[key]
            if key.endswith('gpio') or key.startswith('pinctrl-') and key != 'pinctrl-names':
                if len(value) % 4:
                    raise ValueError('Invalid cell length for ' + key)
                cells = list(struct.unpack('>' + 'I' * (len(value)//4), value))
                selected[key] = {'cells': cells, 'unresolved_overlay_fixup': 0xffffffff in cells}
            else:
                selected[key] = value.rstrip(b'\0').decode('ascii').split('\0')
        fixups = {}
        for key, raw in nodes.get('/__fixups__', {}).items():
            locations = raw.rstrip(b'\0').decode('ascii').split('\0')
            hits = [s for s in locations if name + ':' in s]
            if hits:
                fixups[key] = hits
        result.append({'node': name, 'properties': selected, 'external_fixups': fixups})
    return {'source': str(path), 'fingerprint_nodes': result,
            'classification': 'configuration evidence only; not a live match or power probe'}

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('dtbs', nargs='+', type=Path)
    args = ap.parse_args()
    try:
        print(json.dumps([inventory(p) for p in args.dtbs], indent=2))
    except (OSError, ValueError, IndexError, struct.error) as error:
        ap.exit(1, 'fingerprint inventory: ' + str(error) + '\n')
if __name__ == '__main__':
    main()
