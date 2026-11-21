#!/usr/bin/env python3
"""Transport reviewed board inputs to a disposable hosted runner, never OS images.

Apple extraction and Touch ID's Apple library are fetched by the shared build.
The archive SHA-256 must be pinned independently in the workflow configuration.
"""
import argparse
import io
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import tarfile
from cache import digest, fingerprint

FIXED = {'inputs/x1slte', 'inputs/tools', 'inputs/toolchain.json', 'userland/hfs-kit.bin', 'jb'}
REQUIRED = FIXED - {'jb'} | {'fw-fingerprint', 'fw-wifi', 'fw-haptic', 'fw-sound', 'fw-sensors'}
MANIFEST = 'hosted-inputs.json'


def allowed(name):
    return name in FIXED or re.fullmatch(r'fw-[a-z0-9-]+|userland/hfs-kit-[1-9][0-9]*\.bin', name)


def verify(root, records):
    if not REQUIRED <= records.keys() or any(not allowed(n) for n in records):
        raise ValueError('missing required inputs or unexpected input paths')
    if (root / 'fw-wifi/network').exists():
        raise ValueError('personal Wi-Fi configuration must not be uploaded')
    for name, expected in records.items():
        if (root / name).is_symlink() or fingerprint({name: root / name}, {}) != expected:
            raise ValueError('reviewed input hash mismatch: ' + name)


def pack(root, output):
    lock = json.loads((root / 'inputs/private-inputs.lock.json').read_text())
    records = {n: v for n, v in lock['inputs'].items() if allowed(n)}
    verify(root, records)
    if output.exists():
        raise ValueError('archive already exists; choose a new path')
    output.parent.mkdir(parents=True, exist_ok=True)
    pending = output.with_suffix(output.suffix + '.part')
    try:
        with tarfile.open(pending, 'w:gz', dereference=False) as archive:
            data = json.dumps({'schema': 1, 'inputs': records}, sort_keys=True).encode()
            info = tarfile.TarInfo(MANIFEST)
            info.size, info.mode = len(data), 0o600
            archive.addfile(info, io.BytesIO(data))
            for name in sorted(records):
                base = root / name
                paths = [base] + (sorted(base.rglob('*')) if base.is_dir() else [])
                for path in paths:
                    # Store each regular file itself; no archive hard-link dependencies.
                    archive.inodes.clear()
                    archive.add(path, arcname=path.relative_to(root).as_posix(), recursive=False)
        verify(root, records)  # refuse inputs changed while reading
        pending.replace(output)
        Path(str(output) + '.sha256').write_text(f'{digest(output)}  {output.name}\n')
    finally:
        pending.unlink(missing_ok=True)
    print('Reviewed board/bootstrap archive: ' + str(output))


def unpack(archive_path, root, sha256):
    root = root.resolve()
    if not re.fullmatch('[0-9a-f]{64}', sha256) or digest(archive_path) != sha256:
        raise ValueError('archive SHA-256 mismatch')
    with tarfile.open(archive_path, 'r:gz') as archive:
        manifest = json.load(archive.extractfile(MANIFEST))
        records = manifest['inputs']
        if manifest.get('schema') != 1 or not REQUIRED <= records.keys() or any(not allowed(n) for n in records):
            raise ValueError('invalid hosted input manifest')
        members, seen, links = [], set(), set()
        for member in archive.getmembers():
            name = member.name
            p = PurePosixPath(name)
            if name in seen or p.is_absolute() or '..' in p.parts or str(p) != name:
                raise ValueError('duplicate or unsafe archive member')
            seen.add(name)
            if name == MANIFEST:
                continue
            if not any(name == n or name.startswith(n + '/') for n in records):
                raise ValueError('unexpected archive member: ' + name)
            if not (member.isfile() or member.isdir() or member.issym()):
                raise ValueError('unsupported archive member type: ' + name)
            if member.issym():
                links.add(name)
            members.append(member)
        for member in members:
            if any(str(p) in links for p in PurePosixPath(member.name).parents):
                raise ValueError('archive writes through a symlink')
            dest = root / member.name
            if any(p.is_symlink() for p in (dest, *dest.parents)):
                raise ValueError('destination contains a symlink')
            if dest.exists() and not (dest.is_dir() and member.isdir()) and member.name != 'userland/hfs-kit.bin':
                raise ValueError('requires a fresh checkout: ' + member.name)
        # Create all real paths before links. Absolute runtime links in jb are data,
        # never followed by this extractor. Device nodes and hardlinks are refused.
        for member in sorted(members, key=lambda m: (m.issym(), len(PurePosixPath(m.name).parts))):
            dest = root / member.name
            dest.parent.mkdir(parents=True, exist_ok=True)
            if member.isdir():
                dest.mkdir(exist_ok=True)
            elif member.issym():
                dest.symlink_to(member.linkname)
            else:
                with archive.extractfile(member) as src, dest.open('wb') as out:
                    shutil.copyfileobj(src, out)
            if not member.issym():
                os.chmod(dest, member.mode & 0o777)
        verify(root, records)
    print('Hosted board/bootstrap inputs verified; Apple inputs will be fetched by the build')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='action', required=True)
    q = sub.add_parser('pack')
    q.add_argument('--root', type=Path, required=True)
    q.add_argument('--output', type=Path, required=True)
    q = sub.add_parser('unpack')
    q.add_argument('--archive', type=Path, required=True)
    q.add_argument('--root', type=Path, required=True)
    q.add_argument('--sha256', required=True)
    a = p.parse_args()
    try:
        if a.action == 'pack':
            pack(a.root.resolve(), a.output.resolve())
        else:
            unpack(a.archive, a.root.resolve(), a.sha256)
    except (ValueError, OSError, KeyError, tarfile.TarError) as exc:
        p.exit(1, f'hosted-inputs: {exc}\n')
