#!/usr/bin/env python3
"""Audit only firmware-free Restore preflight ZIPs; never clears the source inventory."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile
from audit import SECRET
from manage import ROOT, profile
from twrp_package import META, device_config


def audit_package(path):
    path = Path(path)
    sha = hashlib.sha256(path.read_bytes()).hexdigest()
    sidecar = Path(str(path) + '.sha256').read_text().split()
    if sidecar != [sha, path.name]:
        raise ValueError('package checksum mismatch')
    build = json.loads(Path(str(path) + '.build.json').read_text())
    if (build.get('zip_sha256') != sha or build.get('action') != 'restore'
            or build.get('source_dirty') is not False or build.get('writes_enabled') is not False
            or build.get('hardware_qualified') is not False):
        raise ValueError('requires a clean, non-writing, unqualified Restore build')
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    if build.get('source_commit') != commit:
        raise ValueError('package source commit differs from audited checkout')
    with zipfile.ZipFile(path) as z:
        if len(z.namelist()) != len(META) or set(z.namelist()) != META or z.testzip():
            raise ValueError('unexpected Restore ZIP inventory or CRC')
        contents = {n: z.read(n) for n in META}
    if any(SECRET.search(data) for data in contents.values()):
        raise ValueError('possible credential in package; value redacted')
    config = json.loads(contents['installer.json'])
    if config != device_config(profile(build['device']), 'restore', 'restore', False):
        raise ValueError('Restore config differs from the measured non-writing profile')
    binary = contents['META-INF/com/google/android/update-binary']
    if binary[:6] != b'\x7fELF\x02\x01' or binary[18:20] != b'\xb7\x00':
        raise ValueError('recovery binary must be little-endian ARM64 ELF')
    if 'HARDWARE QUALIFICATION PENDING' not in contents['README.txt'].decode():
        raise ValueError('missing qualification disclosure')
    return {'path': path.name, 'sha256': sha, 'source_commit': build['source_commit'],
            'device': build['device'], 'writes_enabled': False,
            'entries': {n: hashlib.sha256(data).hexdigest() for n, data in sorted(contents.items())}}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    a = p.parse_args()
    paths = sorted(a.directory.glob('*.zip'))
    if not paths:
        p.error('no Restore ZIPs found')
    try:
        result = {'schema': 1, 'scope': 'Restore preflight packages only; full-source publication review remains unresolved',
                  'packages': [audit_package(path) for path in paths]}
    except (ValueError, KeyError, OSError, zipfile.BadZipFile) as e:
        p.exit(1, f'restore audit: {e}\n')
    output = a.directory / 'restore-publication-audit.json'
    output.write_text(json.dumps(result, indent=2) + '\n')
    print(f'Passed: {len(paths)} firmware-free Restore preflight package(s); report: {output}')
