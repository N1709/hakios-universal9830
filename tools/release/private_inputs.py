#!/usr/bin/env python3
"""Hash locally supplied inputs; a lock records identity, not redistribution rights."""
import json
from pathlib import Path
from cache import fingerprint
from manage import ROOT


def inventory():
    paths = ['inputs/x1slte', 'inputs/tools', 'inputs/toolchain.json',
             'sysimage/rootfs', 'sysimage/cryptex', 'sysimage/cryptex-app',
             'sysimage/20F66__iPhone10,3_6', 'sysimage/system.manifest',
             'sysimage/var.skel', 'sysimage/DeviceTree.d22ap.im4p', 'sysimage/extraction.json', 'userland/hfs-kit.bin']
    paths += [str(p.relative_to(ROOT)) for p in sorted(ROOT.glob('userland/hfs-kit-*.bin'))]  # IOS_DATA_GB sizes
    paths += [str(p.relative_to(ROOT)) for p in sorted(ROOT.glob('fw-*')) if p.is_dir()]
    if (ROOT / 'jb').exists():
        paths.append('jb')
    result = {}
    for name in paths:
        p = ROOT / name
        if p.is_symlink():
            raise ValueError(f'private input must be isolated, not a shared symlink: {name}')
        if name.startswith(('fw-', 'inputs/')) and p.is_dir() and any(x.is_symlink() for x in p.rglob('*')):
            raise ValueError(f'firmware/tool inputs must not contain symlinks: {name}')
        result[name] = fingerprint({name: p}, {})
    return result


def write():
    lock = {'schema': 1, 'device': 'x1slte', 'inputs': inventory(),
            'notice': 'Locally supplied inputs. Hashes do not grant redistribution permission.'}
    dest = ROOT / 'inputs/private-inputs.lock.json'
    dest.write_text(json.dumps(lock, indent=2) + '\n')
    print(f'Wrote {dest}')


def verify():
    lock = json.loads((ROOT / 'inputs/private-inputs.lock.json').read_text())
    if lock.get('schema') != 1 or lock.get('device') != 'x1slte' or lock.get('inputs') != inventory():
        raise ValueError('private inputs changed; review and regenerate their lock before building')
