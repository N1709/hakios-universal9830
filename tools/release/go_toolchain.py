#!/usr/bin/env python3
"""Fetch a checksum-pinned Go toolchain for the standalone recovery installer."""
import fcntl
import os
from pathlib import Path
import platform
import shutil
import tarfile
import tempfile
import urllib.request
from cache import digest
from manage import ROOT

VERSION = 'go1.27.4'
ARCHIVES = {
    ('Darwin','arm64'): ('darwin-arm64','91a44a2b8a716600ad94a4f143e156116620db8828086a6e3602291be9bd0bb4'),
    ('Darwin','x86_64'): ('darwin-amd64','2b799377704ef12db39bd254995d5c348c9cc8d86562ca817724390e56c38f43'),
    ('Linux','x86_64'): ('linux-amd64','7226736c2339719cda28fbf3a54d71aa63bcefe376e98cef00034268dab24840'),
    ('Linux','aarch64'): ('linux-arm64','3392582988a273a87ab5043ee1b2c9c17a2c3c00580bf9326c47a2e0aaaeac92'),
}

def ensure():
    host, sha = ARCHIVES[(platform.system(), platform.machine())]
    base = ROOT / '.cache/toolchains'
    base.mkdir(parents=True, exist_ok=True)
    target = base / (VERSION + '-' + host)
    with (base / 'go.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not (target / 'go/bin/go').is_file():
            work = Path(tempfile.mkdtemp(prefix='.go-', dir=base))
            try:
                archive = work / 'go.tar.gz'
                with urllib.request.urlopen(f'https://go.dev/dl/{VERSION}.{host}.tar.gz', timeout=120) as src, archive.open('wb') as dst:
                    shutil.copyfileobj(src, dst)
                if digest(archive) != sha:
                    raise ValueError('Go archive checksum mismatch')
                with tarfile.open(archive) as tar:
                    for member in tar.getmembers():
                        p = Path(member.name)
                        if p.is_absolute() or '..' in p.parts or p.parts[0] != 'go' or not (member.isfile() or member.isdir()):
                            raise ValueError('unexpected Go archive entry')
                    tar.extractall(work)
                archive.unlink()
                os.replace(work, target)
            finally:
                if work.exists(): shutil.rmtree(work)
    return target / 'go/bin/go'

def environment():
    clean = {k: v for k, v in os.environ.items() if k not in ('GOOS', 'GOARCH', 'GOARM', 'GOARM64', 'GOAMD64')}
    return dict(clean, GOMAXPROCS='4', GOMODCACHE=str(ROOT / '.cache/go-mod'), GOENV='off', GOFLAGS='', GOEXPERIMENT='', CGO_ENABLED='0', GOTOOLCHAIN='local', GOPROXY='off', GOSUMDB='off',
                GOCACHE=str(ROOT / '.cache/go-build'), GOPATH=str(ROOT / '.cache/go-path'))

if __name__ == '__main__':
    print(ensure())
