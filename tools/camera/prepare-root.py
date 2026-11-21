#!/usr/bin/env python3
"""Package S20 Camera for clean installs; seed only missing data-volume files."""
import argparse
from pathlib import Path
import plistlib
import shutil

HOOKS = {'RedwoodCameraLenses': ['com.apple.camera'],
         'RedwoodCameraBrightness': ['com.apple.camera', 'com.apple.springboard']}
CACHE = '/private/var/mobile/Library/Caches/com.apple.mediaserverd'

def prepare(root, binaries):
    required = ['libredwood-media.dylib'] + [name+'.dylib' for name in HOOKS]
    for name in required:
        if not (binaries/name).is_file():
            raise SystemExit(f'Missing Camera binary: {binaries/name}; rebuild camera image artifacts')
    (root/'lib').mkdir(parents=True, exist_ok=True)
    shutil.copyfile(binaries/'libredwood-media.dylib', root/'lib/libredwood-media.dylib')
    (root/'lib/libredwood-media.dylib').chmod(0o755)
    seed = root/'seed'; seed.mkdir(exist_ok=True)
    entries = []
    for name, bundles in HOOKS.items():
        shutil.copyfile(binaries/(name+'.dylib'), seed/(name+'.dylib'))
        (seed/(name+'.plist')).write_bytes(plistlib.dumps({'Filter': {'Bundles': bundles}}))
        for extension, mode in [('dylib', '755'), ('plist', '644')]:
            leaf = name+'.'+extension
            entries.append(f'{mode}\t0\t0\t/private/var/jb/usr/lib/TweakInject/{leaf}\t/seed/{leaf}')
    # Empty enable file is intentional; no cached exposure/calibration is shipped.
    for flag in ('redwood-ultrawide-enable','redwood-gw2-enable'):
        (seed/flag).write_text('')
        entries.append(f'644\t501\t501\t{CACHE}/{flag}\t/seed/{flag}')
    manifest = root/'etc/seed.list'
    lines = manifest.read_text().splitlines() if manifest.exists() else []
    destinations = {line.split('\t')[3] for line in entries}
    lines = [line for line in lines if len(line.split('\t')) != 5 or line.split('\t')[3] not in destinations]
    manifest.write_text('\n'.join(lines+entries)+'\n')
    # Create the shared control directory as mobile before init seeds the flag.
    # Otherwise mkdir_parents() would leave it root-owned on a wiped data volume.
    skeleton = root/'etc/var.skel'
    directories = [f'd\t755\t501\t501\t{CACHE}']
    rows = skeleton.read_text().splitlines() if skeleton.exists() else []
    rows = [row for row in rows if row.split('\t')[-1] != CACHE]
    skeleton.write_text('\n'.join(rows+directories)+'\n')
    conf = root/'etc/inject.conf'
    lines = conf.read_text().splitlines()
    for library, process in [('/redwood/lib/libredwood-media.dylib','mediaserverd'),
                             ('/redwood/lib/libredwood-tweaks.dylib','Camera')]:
        for i,line in enumerate(lines):
            fields = line.split()
            if fields and fields[0] == library:
                lines[i] = ' '.join(fields + ([] if process in fields[1:] else [process]))
                break
        else:
            lines.append(library+' '+process)
    if not (root/'lib/libredwood-tweaks.dylib').is_file():
        raise SystemExit('Camera requires root/lib/libredwood-tweaks.dylib')
    conf.write_text('\n'.join(lines)+'\n')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--binaries', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.root, args.binaries)
