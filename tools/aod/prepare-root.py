#!/usr/bin/env python3
"""Install native AOD into an assembled S20 root, without touching user settings."""
import argparse
from pathlib import Path
import shutil

RULE = '/redwood/lib/libredwood-alwayson.dylib SpringBoard Preferences'

def prepare(root, binaries):
    # Refuse incomplete prebuilt sets rather than silently omitting the feature.
    sources = {'lib/libredwood-alwayson.dylib': binaries/'libredwood-alwayson.dylib',
               'bin/aod-preview': binaries/'aod-preview'}
    for source in sources.values():
        if not source.is_file():
            raise SystemExit(f'Missing AOD binary: {source}; rebuild tools/aod/build.sh')
    conf = root/'etc/inject.conf'
    lines = conf.read_text().splitlines()
    # One targeted rule even if assembly is repeated. Preserve every other rule.
    lines = [s for s in lines if not s.split() or s.split()[0] != RULE.split()[0]]
    for target, source in sources.items():
        destination = root/target
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        destination.chmod(0o755)
    conf.write_text('\n'.join(lines + [RULE]) + '\n')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--binaries', required=True, type=Path)
    args = parser.parse_args()
    prepare(args.root, args.binaries)
