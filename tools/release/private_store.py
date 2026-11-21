#!/usr/bin/env python3
"""Private persistent-volume adapters for disposable image CI workers.

Stage only the known private-input paths; retain images by SHA-256 so unchanged
large files have one stored copy across candidates. No network/public uploads.
The storage volume must be restricted to the private build service account.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
from cache import Cache, digest
from manage import ROOT


def stage(snapshot):
    snapshot = snapshot.resolve()
    if snapshot == ROOT or ROOT in snapshot.parents or snapshot in ROOT.parents:
        raise ValueError('snapshot must be outside the checkout')
    lock = json.loads((snapshot / 'inputs/private-inputs.lock.json').read_text())
    allowed = {'inputs/x1slte', 'inputs/tools', 'inputs/toolchain.json',
               'sysimage/rootfs', 'sysimage/cryptex', 'sysimage/cryptex-app',
               'sysimage/20F66__iPhone10,3_6', 'sysimage/system.manifest',
               'sysimage/var.skel', 'sysimage/DeviceTree.d22ap.im4p', 'sysimage/extraction.json', 'userland/hfs-kit.bin', 'jb'}
    import re
    names = list(lock['inputs']) + ['inputs/private-inputs.lock.json']
    for name in names:
        if name not in allowed and name != 'inputs/private-inputs.lock.json' and not re.fullmatch(
                r'fw-[a-z0-9-]+|userland/hfs-kit-[1-9][0-9]*\.bin', name):
            raise ValueError(f'input path not allowed: {name}')
        src, dst = snapshot / name, ROOT / name
        if src.is_symlink() or not src.resolve().is_relative_to(snapshot):
            raise ValueError(f'input escapes snapshot: {name}')
        if dst.exists() and name != 'userland/hfs-kit.bin':
            raise ValueError(f'requires a disposable checkout, destination already exists: {name}')
        dst.parent.mkdir(parents=True, exist_ok=True)
        if src.is_dir():
            shutil.copytree(src, dst, symlinks=True)
        else:
            shutil.copy2(src, dst)
    from private_inputs import verify
    verify()
    print('Private snapshot copied and content lock verified')


def retain(bundle, store):
    from flash_plan import verify
    manifest, _ = verify(bundle)
    store = store.resolve()
    if store == ROOT or ROOT in store.parents:
        raise ValueError('persistent store must be outside the checkout')
    cache = Cache(store / 'blobs')
    for name, meta in manifest['files'].items():
        cache.store(meta['sha256'], bundle / name)
        # A damaged existing object must fail retention instead of being trusted.
        entry = cache.root / meta['sha256']
        if digest(entry / 'payload') != meta['sha256']:
            raise ValueError(f'private stored object is corrupt: {meta["sha256"]}')
    key = digest(bundle / 'manifest.json')
    candidates = store / 'candidates'
    candidates.mkdir(parents=True, exist_ok=True, mode=0o700)
    target = candidates / key
    if not target.exists():
        tmp = Path(tempfile.mkdtemp(prefix='.pending-', dir=candidates))
        try:
            for name in ('manifest.json', 'SHA256SUMS'):
                shutil.copyfile(bundle / name, tmp / name)
            os.replace(tmp, target)
        finally:
            if tmp.exists():
                shutil.rmtree(tmp)
    print(f'Private candidate retained: {key}; unchanged images share existing objects')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    s = p.add_subparsers(dest='action', required=True)
    q = s.add_parser('stage')
    q.add_argument('--snapshot', type=Path, required=True)
    q = s.add_parser('retain')
    q.add_argument('--bundle', type=Path, required=True)
    q.add_argument('--store', type=Path, required=True)
    a = p.parse_args()
    try:
        if a.action == 'stage':
            stage(a.snapshot)
        else:
            retain(a.bundle, a.store)
    except (OSError, ValueError, KeyError) as exc:
        sys.exit(f'private-store: {exc}')
