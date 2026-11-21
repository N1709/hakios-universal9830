#!/usr/bin/env python3
"""Content receipt for the pinned Apple extraction and its selection recipe."""
import json
from pathlib import Path
from cache import fingerprint

OUTPUTS = ('rootfs', 'cryptex', 'cryptex-app', '20F66__iPhone10,3_6',
           'system.manifest', 'var.skel')
RECIPES = ('sysimage/fetch.sh', 'sysimage/fetch-from-ipsw.py',
           'sysimage/select-system.py', 'sysimage/mkvarskel.py',
           'tools/font_trim.py', 'tools/release/extraction.py')

def recipe(root):
    return fingerprint({p: root / p for p in RECIPES}, {'build': '20F66'})

def contents(root):
    return fingerprint({p: root / 'sysimage' / p for p in OUTPUTS}, {})

def write(root):
    data = {'schema': 1, 'recipe': recipe(root), 'contents': contents(root)}
    target = root / 'sysimage/extraction.json'
    tmp = target.with_suffix('.json.part')
    tmp.write_text(json.dumps(data, indent=2) + '\n')
    tmp.replace(target)

def verify(root):
    p = root / 'sysimage/extraction.json'
    if p.is_symlink() or not p.is_file():
        raise ValueError('missing extraction receipt; run build.sh local to prepare Apple inputs')
    data = json.loads(p.read_text())
    if data != {'schema': 1, 'recipe': recipe(root), 'contents': contents(root)}:
        raise ValueError('extraction or its recipe changed; run build.sh local to prepare Apple inputs')

if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['write', 'verify'])
    a = p.parse_args()
    (write if a.action == 'write' else verify)(Path(__file__).resolve().parents[2])
