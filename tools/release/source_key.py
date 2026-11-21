#!/usr/bin/env python3
"""Kernel source identity, independent of checkout location and documentation."""
from pathlib import Path
import subprocess
from cache import fingerprint

root = Path(__file__).resolve().parents[2]
paths = (
    'xnu/redwood.patch', 'xnu/files', 'xnu/apply.sh', 'xnu-deps/setup.sh',
    'xnu-deps/host', 'xnu-deps/hostinc', 'xnu-deps/iig-impl',
    'xnu/xnu-env.sh', 'xnu/xnu-build.sh', 'common/macos-env.sh', 'ci/sources.lock.json',
    'ci/build-kernel.sh', 'tools/release/source_key.py', 'tools/release/cache.py',
    'tools/release/kernel_manifest.py')
inputs = {}
for name in paths:
    p = root / name
    if p.is_dir():
        # setup.sh populates host/bin with generated tools and absolute links.
        # Only tracked source files belong in the source identity.
        names = subprocess.check_output(['git', 'ls-files', '-z', '--', name], cwd=root).decode().split('\0')
        for item in filter(None, names):
            inputs[item] = root / item
    else:
        inputs[name] = p
print(fingerprint(inputs, {'GIC_V2': 1}))
