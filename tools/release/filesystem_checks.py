#!/usr/bin/env python3
"""Read-only native filesystem checks on private image files, never phone devices."""
from pathlib import Path
import plistlib
import re
import shutil
import struct
import subprocess
import tempfile
from images import apfs, hfs_kit
from manage import ROOT


def run(*args):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print(result.stdout, end='', flush=True)
    if result.returncode:
        raise ValueError(f'filesystem validation failed ({result.returncode}): {args[0]}')


def verify(recovery, system, kit, device):
    for tool in ('/sbin/fsck_apfs', '/sbin/fsck_hfs', '/usr/bin/hdiutil'):
        if not Path(tool).is_file():
            raise ValueError('native macOS filesystem validation tool required: ' + tool)
    hfs_kit(kit, __import__('targets').data_bytes(device['partitions']['userdata']))
    work_root = ROOT / 'out/filesystem-checks'
    work_root.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='check-', dir=work_root))
    attached = None
    preserve = False
    try:
        offset = device['layout']['root_offset']
        size = apfs(recovery, device['partitions']['recovery'] - offset - 64, offset)
        root = work / 'root.apfs'
        with recovery.open('rb') as source, root.open('xb') as target:
            source.seek(offset)
            remaining = size
            while remaining:
                data = source.read(min(remaining, 1024 * 1024))
                if not data:
                    raise ValueError('truncated recovery root')
                target.write(data)
                remaining -= len(data)
        run('/sbin/fsck_apfs', '-n', str(root))
        run('/sbin/fsck_apfs', '-n', str(system))
        data = kit.read_bytes()
        block, entries, capacity, _, _ = struct.unpack_from('<IIQII', data, 8)
        at = 32 + entries * 16
        raw = work / 'hfs-seed.raw'
        # A new sparse regular file: only metadata data extents allocate storage;
        # zero extents read as holes. No existing disk is opened for formatting.
        with raw.open('xb') as f:
            f.truncate(capacity)
            for i in range(entries):
                first, count, kind = struct.unpack_from('<QII', data, 32 + i * 16)
                if kind:
                    size = count * block
                    f.seek(first * block)
                    f.write(data[at:at + size])
                    at += size
        before = {str(p) for p in Path('/dev').glob('disk*')}
        result = subprocess.run(['/usr/bin/hdiutil', 'attach', '-nomount', '-readonly',
                                 '-imagekey', 'diskimage-class=CRawDiskImage', '-plist', str(raw)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode:
            raise ValueError('cannot attach private read-only HFS test image: ' + result.stderr.decode(errors='replace'))
        entities = plistlib.loads(result.stdout)['system-entities']
        new = [e['dev-entry'] for e in entities
               if re.fullmatch(r'/dev/disk\d+', e.get('dev-entry', '')) and e['dev-entry'] not in before]
        if len(new) != 1:
            preserve = True
            raise ValueError(f'unexpected virtual-image attachment; preserve {work} for inspection')
        attached = new[0]
        run('/sbin/fsck_hfs', '-fn', attached.replace('/dev/disk', '/dev/rdisk'))
        print('Native APFS root/system and reconstructed HFSX seed checks passed', flush=True)
    finally:
        if attached is not None:
            result = subprocess.run(['/usr/bin/hdiutil', 'detach', attached], capture_output=True, text=True)
            print(result.stdout, end='', flush=True)
            if result.returncode:
                preserve = True
                raise ValueError(f'own read-only virtual image did not detach: {attached}; retained {work}: {result.stderr}')
        if not preserve:
            shutil.rmtree(work)
