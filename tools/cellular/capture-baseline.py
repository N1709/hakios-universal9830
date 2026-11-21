#!/usr/bin/env python3
"""Capture current BOOT/recovery/kernel under the phone lock; phone reads only.

Run in a visible Terminal job. Never reuses an output directory. Private firmware
and kernel identity stay in ignored out/cellular; stdout contains metadata only.
The resulting baseline is not authorization to deploy: compare again just before
writing, because another session may update the phone after capture.
"""
import argparse
import datetime
import importlib.util
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
RESEARCH = ROOT.parent.parent
spec = importlib.util.spec_from_file_location('prepare', Path(__file__).with_name('prepare-m0.py'))
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


def validate_pair(recovery, boot):
    _, size = prepare.boot_layout(recovery)
    prepare.require(len(boot) == size and boot == recovery[:size],
                    'BOOT/recovery prefix mismatch; do not build a fallback from this pair')
    records = prepare.entries(recovery)
    names = [name.rstrip(b'\0') for name, _, _ in records]
    prepare.require(names.count(b'loader.efi') == 1 and
                    not set(names).intersection({b'fallback.efi', b'probe.report', b'cp.boot', b'cp.boot.info'}),
                    'expected a normal installed loader, not an existing probe')
    return dict(recovery_sha256=prepare.sha(recovery), boot_sha256=prepare.sha(boot),
                recovery_size=len(recovery), boot_size=size,
                installed_loader_sha256=prepare.sha(next(data for name, _, data in records
                                                        if name.rstrip(b'\0') == b'loader.efi')))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    if not output.is_relative_to((ROOT / 'out/cellular').resolve()):
        parser.error('private backup output must be inside this worktree out/cellular')
    owner = RESEARCH / 'build/phone.lock/owner'

    def lock():
        if not owner.exists() or owner.read_text().split()[0] != os.environ.get('CELLULAR_SESSION', 'codex-cellular'):
            raise RuntimeError('phone lock is not held by codex-cellular; no further reads')

    ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', 's20']

    def read(command, timeout=30):
        lock()
        result = subprocess.run(ssh + [command], capture_output=True, timeout=timeout)
        lock()
        if result.returncode:
            raise RuntimeError('phone read failed; partial output is not a usable baseline')
        return result.stdout

    lock()
    os.umask(0o077)
    output.mkdir(parents=True, exist_ok=False)
    kernel = read('uname -a')
    print('Reading current recovery; no phone writes.', flush=True)
    recovery = read('dd if=/dev/rufs_recovery bs=4096 count=16848', 180)
    _, size = prepare.boot_layout(recovery)
    boot = read(f'dd if=/dev/rufs_boot bs=4096 count={size // 4096}')
    metadata = validate_pair(recovery, boot)
    # Re-read the full recovery to detect an in-progress replacement, not just
    # a stable kernel name. All firmware bytes remain local/private.
    print('Checking a second recovery read for consistency.', flush=True)
    if read('dd if=/dev/rufs_recovery bs=4096 count=16848', 180) != recovery:
        raise RuntimeError('recovery changed during capture')
    if read('uname -a') != kernel or read(f'dd if=/dev/rufs_boot bs=4096 count={size // 4096}') != boot:
        raise RuntimeError('kernel/BOOT changed during capture')
    recovery_path = output / 'recovery.img'
    recovery_path.write_bytes(recovery)
    prepare.verify(RESEARCH / 'tools/bin/avbtool.py', RESEARCH / 'tools/dl/avb/testkey_rsa4096.pem', recovery_path)
    lock()
    (output / 'boot-prefix.bin').write_bytes(boot)
    (output / 'kernel.txt').write_bytes(kernel)
    metadata.update(captured_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    kernel_sha256=prepare.sha(kernel), avb_verified=True,
                    scope='current capture only; revalidate installed bytes before deployment')
    # Written last: absence of this file means capture did not complete.
    (output / 'baseline.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print('Baseline captured, consistent and AVB verified. No phone state changed.', flush=True)


if __name__ == '__main__':
    main()
