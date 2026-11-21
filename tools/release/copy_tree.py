#!/usr/bin/env python3
"""Copy mutable image staging trees without sharing writable inodes with inputs."""
import argparse
from pathlib import Path
import platform
import shutil
import subprocess


def copy_tree(source, destination):
    source, destination = Path(source), Path(destination)
    if source.is_symlink() or not source.is_dir():
        raise ValueError('source must be a real directory, not a shared directory symlink')
    if destination.exists() or destination.is_symlink():
        raise ValueError(f'destination must not exist: {destination}')
    if source.resolve() in destination.resolve().parents:
        raise ValueError('destination must not be inside the source')
    destination.parent.mkdir(parents=True, exist_ok=True)
    if platform.system() == 'Darwin':
        # clonefile copies on APFS: contents are shared until written, inodes are independent.
        subprocess.run(['/bin/cp', '-cRp', str(source), str(destination)], check=True)
    else:
        # Independent ordinary copies are correct on filesystems without clonefile.
        shutil.copytree(source, destination, symlinks=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('destination', type=Path)
    a = p.parse_args()
    copy_tree(a.source, a.destination)
