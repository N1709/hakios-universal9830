#!/usr/bin/env python3
"""Called *after compilation* by userland/build.sh to cache system.apfs only.

Conservative dependency set: the entire selected IPSW tree and image recipe,
actual system executables, optional firmware/apps/bootstrap and explicit options.
Hashing several GB is intentional; directory mtimes cannot detect nested edits.
"""
import argparse
import json
import os
from pathlib import Path
import sys
import subprocess
import platform
import zlib
from cache import Cache, fingerprint


def key(root, ios):
    inputs = {
        'recipe': root / 'userland/build.sh',
        'cache-recipe': Path(__file__),
        'cache-engine': Path(__file__).with_name('cache.py'),
        'copy-tree': Path(__file__).with_name('copy_tree.py'),
        'provider': root / 'tools/storage/stage-provider.py',
        'stage0': root / 'userland/obj/stage0',
        'LocalFilesHost': root / 'userland/obj/LocalFilesHost',
    }
    for name in filter(None, subprocess.check_output(
            ['git', 'ls-files', '-z', '--', 'tools/apfs'], cwd=root).decode().split('\0')):
        inputs[name] = root / name
    for name in ('rootfs', 'cryptex', 'cryptex-app', '20F66__iPhone10,3_6', 'system.manifest'):
        inputs['ios/' + name] = ios / name
    optional = ('fw-audio', 'fw-sound', 'jb/root/var/jb', 'apps',
                'userland/obj/RedwoodTriangle.app')
    for name in optional:
        p = root / name
        if p.exists():
            if p.is_symlink():
                raise ValueError(f'private input must be an isolated directory, not a symlink: {p}')
            if name in ('fw-audio', 'fw-sound', 'apps') and any(x.is_symlink() for x in p.rglob('*')):
                raise ValueError(f'inputs copied with dereferencing must contain no symlinks: {p}')
            inputs[name] = p
    toolchain = root / 'inputs/toolchain.json'
    if toolchain.exists():
        inputs['toolchain'] = toolchain
    # All build options accepted by the system stage; future recipe edits invalidate the key.
    return fingerprint(inputs, {'stage': 'system-v1', 'SILEO_BIN': 'chmod cp mkdir mv rm',
                                'present': sorted(inputs), 'python': platform.python_version(),
                                'zlib': zlib.ZLIB_VERSION})


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', choices=['restore', 'store'])
    p.add_argument('--ios', required=True, type=Path)
    p.add_argument('--force', action='store_true', help='rebuild even on an exact cache hit')
    a = p.parse_args()
    root = Path(__file__).resolve().parents[2]
    cache = Cache(os.environ['HACKIOS_IMAGE_CACHE'])
    stamp = root / 'userland/obj/system-cache.json'
    image = root / 'userland/system.apfs'
    if a.action == 'restore':
        k = key(root, a.ios.resolve())
        stamp.write_text(json.dumps({'key': k}) + '\n')
        hit = False if a.force else cache.restore(k, image)
        print(f'system image cache: {"hit" if hit else "miss"} {k}')
        return 0 if hit else 10
    k = json.loads(stamp.read_text())['key']
    if key(root, a.ios.resolve()) != k:
        raise ValueError('system inputs changed during the build; refusing cache publication')
    cache.store(k, image)
    print(f'system image cache: saved {k}')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, KeyError) as exc:
        sys.exit(f'system-cache: {exc}')
