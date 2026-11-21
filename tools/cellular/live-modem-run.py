#!/usr/bin/env python3
"""One live BOOT-only modem attempt, always followed by CP-off cleanup. No reboot."""
import argparse
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
RESEARCH = ROOT.parent.parent


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True, help='private successfully installed live base')
    parser.add_argument('--radio', type=Path, required=True, help='local read-only RADIO backup; extracts BOOT only')
    parser.add_argument('--link', action='store_true', help='after BOOT, one bounded Gen1 PCIe endpoint attempt')
    args = parser.parse_args()
    base = args.base.resolve()
    if not base.is_relative_to((ROOT / 'out/cellular').resolve()):
        parser.error('base must be under this worktree out/cellular')
    os.umask(0o077)
    owner = RESEARCH / 'build/phone.lock/owner'

    def lock():
        if not owner.exists() or owner.read_text().split()[0] != os.environ.get('CELLULAR_SESSION', 'codex-cellular'):
            raise RuntimeError('cellular must hold the phone lock')

    lock()
    certificate = json.loads((base / 'installed.json').read_text())
    if not all(certificate.get(key) is True for key in ('installed', 'spi8_api', 'loader_reserved_three')):
        raise RuntimeError('live base has no completed installation certificate')
    baseline = json.loads((base / 'baseline.json').read_text())
    boot_size = baseline['boot_size']
    if boot_size <= 0 or boot_size % 4096:
        raise RuntimeError('invalid BOOT prefix size')
    candidate = base / 'reserved-spi8-candidate'
    if sha((candidate / 'standalone-boot-head.bin').read_bytes()) != certificate['boot_sha256']:
        raise RuntimeError('candidate does not match installed certificate')
    ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', 's20']

    def read(command, timeout=20):
        lock()
        result = subprocess.run(ssh + [command], capture_output=True, timeout=timeout, check=True)
        lock()
        return result.stdout

    subprocess.run([sys.executable, str(Path(__file__).with_name('battery-gate.py')),
                    '--require-ready'], check=True)
    if read('/var/root/rwsys kern.redwood.poke "spi8.rx 0"').strip() != b'spi8: invalid byte count/hex':
        raise RuntimeError('running kernel lacks SPI8 hooks')
    if sha(read(f'dd if=/dev/rufs_boot bs=4096 count={boot_size // 4096}', 40)) != certificate['boot_sha256']:
        raise RuntimeError('installed BOOT changed; recapture before any modem attempt')
    segment = (candidate / 'kernel-segment.bin').read_bytes()
    if read(f'dd if=/dev/rufs_recovery bs=4096 skip=8192 count={len(segment) // 4096}', 60) != segment:
        raise RuntimeError('installed kernel segment changed')
    loader_log = read('dd if=/dev/rufs_super bs=4096 skip=2001668 count=32', 20).split(b'\0', 1)[0]
    if (b'cellular memory: reserved 3/3' not in loader_log or
            b'cellular memory: normal-nc 3/3' not in loader_log or
            loader_log != (base / 'reserved-loader-log.txt').read_bytes()):
        raise RuntimeError('loader evidence changed; review CP reservations before power')
    spec = importlib.util.spec_from_file_location('prepare', Path(__file__).with_name('prepare-m0.py'))
    prepare = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(prepare)
    data, info, metadata = prepare.cp_boot_payload(args.radio)
    if metadata['cp_address'] != '0x40000000' or metadata['entry_id'] != 1:
        raise RuntimeError('unexpected BOOT target metadata')
    binary = ROOT / 'out/cellular/live/live-modem'
    digest = sha(binary.read_bytes())
    subprocess.run(['codesign', '--verify', '--strict', str(binary)], check=True)
    out = ROOT / 'logs/cellular' / ('live-modem-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    out.mkdir(parents=True)
    (out / 'loader-evidence.bin').write_bytes(loader_log)
    target = '/var/root/cellular-live-modem-' + digest[:12]
    with tempfile.TemporaryDirectory(prefix='cellular-boot-') as directory:
        payload, meta = Path(directory) / 'boot.bin', Path(directory) / 'boot.info'
        payload.write_bytes(data)
        meta.write_bytes(info)
        for source, remote in ((binary, target), (payload, target + '.boot'), (meta, target + '.info')):
            lock()
            subprocess.run(['scp', '-q', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', str(source),
                            's20:' + remote], check=True, timeout=30)
    read('chmod 700 ' + shlex.quote(target))
    read('chmod 600 ' + shlex.quote(target + '.boot') + ' ' + shlex.quote(target + '.info'))
    result = dict(mode='BOOT+PCIe endpoint attempt; shutdown' if args.link else 'BOOT-only; shutdown after observation', binary_sha256=digest,
                  boot_metadata=metadata, installed_boot_sha256=certificate['boot_sha256'],
                  reboot_requested=False, status='started; CP power may be requested')
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    command = 'exec ' + shlex.join([target, '--reserved-base-verified', target + '.boot', target + '.info'] +
                                        (['--link'] if args.link else []))
    lock()
    print('One live modem BOOT attempt; no reboot. Evidence:', out, flush=True)
    try:
        response = subprocess.run(ssh + [command], capture_output=True, timeout=45)
    except subprocess.TimeoutExpired as error:
        (out / 'transcript.txt').write_bytes((error.stdout or b'') + (error.stderr or b''))
        result.update(status='timeout; remote state unknown; do not repeat')
        (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print('Timed out. Inspect phase and CP shutdown before any further action.', flush=True)
        return 2
    transcript = response.stdout + response.stderr
    (out / 'transcript.txt').write_bytes(transcript)
    print(transcript.decode(errors='replace'), end='', flush=True)
    safe = b'cleanup_verified 1;' in transcript
    passed = response.returncode == 0 and safe
    if args.link:
        passed = passed and b'PCIe error 0 ' in transcript
    result.update(exit_code=response.returncode, cleanup_confirmed=safe,
                  status=('PCIe endpoint observed; remaining firmware pending' if args.link else 'transport complete; modem acceptance unproven') if passed else 'failed; inspect transcript')
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if safe:
        read('rm -f ' + shlex.join([target, target + '.boot', target + '.info']))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
