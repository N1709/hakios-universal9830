#!/usr/bin/env python3
"""Verify AOD (default on, wallpaper off) ships in clean S20 root assembly with no data tweaks."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('aod_prepare', Path(__file__).with_name('prepare-root.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
binaries = repo/'prebuilt/userland'
with tempfile.TemporaryDirectory(prefix='aod-assembly-') as tmp:
    root = Path(tmp)
    (root/'etc').mkdir()
    original = '/redwood/lib/libredwood-sb.dylib SpringBoard\n/other/lib.dylib Preferences\n'
    (root/'etc/inject.conf').write_text(original)
    for _ in range(2):
        subprocess.run(['python3', str(repo/'tools/aod/prepare-root.py'), str(root),
                        '--binaries', str(binaries)], check=True)
    conf = (root/'etc/inject.conf').read_text()
    assert conf == original + module.RULE + '\n'
    assert (root/'lib/libredwood-alwayson.dylib').read_bytes() == (binaries/'libredwood-alwayson.dylib').read_bytes()
    assert (root/'bin/aod-preview').read_bytes() == (binaries/'aod-preview').read_bytes()
    assert not (root/'var').exists(), 'Assembly must not seed enabled preferences or depend on data tweaks'
    # Incomplete source sets must fail before changing assembly.
    before = (root/'etc/inject.conf').read_bytes()
    bad = root/'missing-binaries'
    bad.mkdir()
    r = subprocess.run(['python3', str(repo/'tools/aod/prepare-root.py'), str(root),
                        '--binaries', str(bad)], capture_output=True)
    assert r.returncode and (root/'etc/inject.conf').read_bytes() == before
binary = (binaries/'libredwood-alwayson.dylib').read_bytes()
assert b'RWAlwaysOnImageController' in binary and b'RWAlwaysOnController\0' not in binary
assert b'redwoodAlwaysOnServiceInstalled' in binary
build = (repo/'userland/build.sh').read_text()
assert 'python3 ../tools/aod/prepare-root.py root --binaries obj' in build
print('PASS: independent, idempotent S20 AOD root assembly, preserved injection rules, no seeded preferences, duplicate guard and distinct Settings class')
