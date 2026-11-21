#!/usr/bin/env python3
"""Private, content-addressed file cache. Never use a public Actions cache for images.

Entries are immutable, checksummed and atomically published. A key includes input
names, contents, modes, symlink targets and explicit build options; not mtimes.
Readers/writers serialize per key. Outputs must be regular files, never devices.
"""
import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import tempfile


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(8 * 1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def fingerprint(inputs, options):
    """Input labels are stable across workspaces; paths may be absolute."""
    h = hashlib.sha256(json.dumps({'version': 1, 'options': options},
                                 sort_keys=True, separators=(',', ':')).encode())

    def record(label, path):
        if path.is_symlink():
            info = [label, 'link', os.readlink(path)]
        elif path.is_file():
            info = [label, 'file', stat.S_IMODE(path.stat().st_mode), digest(path)]
        elif path.is_dir():
            info = [label, 'dir', stat.S_IMODE(path.stat().st_mode)]
        else:
            raise ValueError(f'input missing or not a regular file/directory: {path}')
        h.update(json.dumps(info, separators=(',', ':')).encode() + b'\n')
        if path.is_dir() and not path.is_symlink():
            for child in sorted(path.iterdir()):
                record(label + '/' + child.name, child)

    for label, path in sorted(inputs.items()):
        record(label, Path(path))
    return h.hexdigest()


class Cache:
    def __init__(self, root):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)

    @contextlib.contextmanager
    def lock(self, key):
        if not re.fullmatch('[a-f0-9]{64}', key):
            raise ValueError('invalid cache key')
        with (self.root / (key + '.lock')).open('a') as f:
            fcntl.flock(f, fcntl.LOCK_EX)
            yield self.root / key

    @staticmethod
    def check_destination(path):
        p = Path(path)
        if p.is_symlink() or (p.exists() and not p.is_file()):
            raise ValueError(f'output must be a regular file: {p}')
        p.parent.mkdir(parents=True, exist_ok=True)
        return p

    def restore(self, key, destination):
        dest = self.check_destination(destination)
        with self.lock(key) as entry:
            if not entry.exists():
                return False
            meta = json.loads((entry / 'manifest.json').read_text())
            payload = entry / 'payload'
            if (meta.get('key') != key or payload.is_symlink() or
                    payload.stat().st_size != meta.get('size') or
                    digest(payload) != meta.get('sha256')):
                raise ValueError(f'corrupt cache entry: {entry}; quarantine it before retrying')
            fd, name = tempfile.mkstemp(prefix='.restore-', dir=dest.parent)
            os.close(fd)
            try:
                shutil.copyfile(payload, name)
                os.replace(name, dest)
            finally:
                Path(name).unlink(missing_ok=True)
        return True

    def store(self, key, source):
        source = Path(source)
        if source.is_symlink() or not source.is_file() or source.stat().st_size == 0:
            raise ValueError('cache source must be a nonempty regular file')
        with self.lock(key) as entry:
            if entry.exists():
                # A concurrent build already published this key. Validate it on restore.
                return
            tmp = Path(tempfile.mkdtemp(prefix='.pending-', dir=self.root))
            try:
                shutil.copyfile(source, tmp / 'payload')
                meta = {'schema': 1, 'key': key, 'size': (tmp / 'payload').stat().st_size,
                        'sha256': digest(tmp / 'payload')}
                (tmp / 'manifest.json').write_text(json.dumps(meta, sort_keys=True) + '\n')
                os.replace(tmp, entry)
            finally:
                if tmp.exists():
                    shutil.rmtree(tmp)
