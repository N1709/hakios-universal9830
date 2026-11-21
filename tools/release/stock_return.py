#!/usr/bin/env python3
"""Read-only stock firmware inspection and return-to-Android planning. Never flashes."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tarfile
from manage import profile


def inspect(path, role):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f'not a regular firmware archive: {path}')
    size = path.stat().st_size
    payload = size
    expected = None
    if path.name.endswith('.tar.md5'):
        with path.open('rb') as f:
            f.seek(max(0, size - 4096))
            tail = f.read()
        match = re.search(rb'([0-9a-fA-F]{32})(?:[ \t]+[^\r\n]*)?\r?\n?\Z', tail)
        if match is None:
            raise ValueError(f'missing Odin MD5 trailer: {path.name}')
        payload = max(0, size - 4096) + match.start()
        if payload % 512 or payload == 0:
            raise ValueError(f'misaligned Odin MD5 trailer: {path.name}')
        expected = match.group(1).decode().lower()
    full = hashlib.sha256()
    md5 = hashlib.md5()  # Odin transport corruption check, never publisher authentication.
    at = 0
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(8 * 1024 * 1024), b''):
            full.update(chunk)
            if at < payload:
                md5.update(chunk[:payload - at])
            at += len(chunk)
    if expected is not None and md5.hexdigest() != expected:
        raise ValueError(f'Odin MD5 mismatch: {path.name}')
    names = []
    # Odin uses flat regular-file TAR members. Parse fixed headers ourselves to
    # reject links/PAX extensions without allowing unbounded metadata allocation.
    with path.open('rb') as archive:
        at = 0
        ended = False
        while at + 512 <= payload:
            archive.seek(at)
            header = archive.read(512)
            if header == bytes(512):
                remaining = payload - at
                archive.seek(at)
                if remaining < 1024:
                    raise ValueError('truncated TAR end marker')
                while remaining:
                    block = archive.read(min(remaining, 1024 * 1024))
                    if not block or any(block):
                        raise ValueError('nonzero data after TAR end marker')
                    remaining -= len(block)
                ended = True
                break
            entry = tarfile.TarInfo.frombuf(header, 'utf-8', 'strict')
            name = Path(entry.name)
            if (not entry.isfile() or name.is_absolute() or '..' in name.parts or
                    '\\' in entry.name or entry.size < 0 or at + 512 + entry.size > payload or entry.name in names):
                raise ValueError(f'unsafe, duplicate or truncated TAR member in {path.name}')
            names.append(entry.name)
            if len(names) > 4096:
                raise ValueError('too many firmware members')
            at += 512 + (entry.size + 511) // 512 * 512
        if not ended:
            raise ValueError('TAR end marker missing')
    if not names:
        raise ValueError('empty firmware archive')
    base = [Path(n).name for n in names]
    image = lambda stem: any(re.fullmatch(re.escape(stem) + r'(?:\.\d+)?(?:\.lz4)?', n) for n in base)
    if role == 'AP' and not all(image(n) for n in ('boot.img', 'recovery.img', 'super.img', 'vbmeta.img')):
        raise ValueError('AP must contain full stock boot, recovery, super and vbmeta; boot-only/AP patches cannot restore Android')
    if role == 'BL' and not (image('sboot.bin') or image('bootloader.img')):
        raise ValueError('BL bootloader component missing')
    if role == 'CP' and not image('modem.bin'):
        raise ValueError('CP modem component missing')
    return {'file': path.name, 'bytes': size, 'sha256': full.hexdigest(),
            'odin_md5_checked': expected is not None, 'members': names,
            'embedded_pit': [n for n in names if n.lower().endswith('.pit')]}


def plan(directory, device='x1slte'):
    d = profile(device)
    if device != 'x1slte':
        raise ValueError('stock return plan is only defined for the measured x1slte profile')
    code = d['models'][0].removeprefix('SM-')
    files = {}
    for role in ('BL', 'AP', 'CP', 'CSC'):
        matches = sorted(p for p in directory.iterdir() if p.name.startswith(role + '_') and
                         (p.name.endswith('.tar.md5') or p.name.endswith('.tar')))
        if len(matches) != 1:
            raise ValueError(f'exactly one {role}_*.tar[.md5] required; use CSC, not HOME_CSC')
        if not re.search(r'(?:^|_)' + re.escape(code) + r'[A-Z0-9_]*', matches[0].name):
            raise ValueError(f'{role} filename is not for {code}')
        files[role] = inspect(matches[0], role)
    # Initial plan only accepts the exact recorded firmware revision; no guessed anti-rollback arithmetic.
    for role in ('BL', 'AP'):
        if not files[role]['file'].startswith(role + '_' + d['bootloaders'][0] + '_'):
            raise ValueError('BL/AP revision differs from recorded firmware; separately qualify any newer firmware, never force downgrade')
    return {'schema': 1, 'device': device, 'executable': False, 'files': files,
            'manual_pit_repartition_required_by_our_installer': False,
            'data_preservation': False,
            'process': ['Use verified off-device backups and working Download Mode',
                        'Confirm genuine matching stock firmware and bootloader compatibility independently',
                        'Restore full stock BL/AP/CP/CSC using the qualified Odin procedure; do not use HOME_CSC',
                        'Boot stock recovery and factory reset userdata to Android filesystem/encryption',
                        'Verify Android cold boot and storage; retain backups and keep bootloader unlocked'],
            'notice': 'Filename/member/transport integrity checks only. Not Samsung signature/PIT validation or hardware qualification. No flashing commands executed.'}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--firmware', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    a = p.parse_args()
    result = plan(a.firmware.resolve())
    with a.output.open('x') as f:
        json.dump(result, f, indent=2)
        f.write('\n')
    print(f'Read-only return-to-stock plan: {a.output}; no device operations')

if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, tarfile.TarError) as e:
        raise SystemExit(f'stock-plan: {e}')
