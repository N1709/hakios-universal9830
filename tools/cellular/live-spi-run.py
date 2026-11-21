#!/usr/bin/env python3
"""Run internal SPI loopback live; no reboot, firmware or CP control writes."""
import os
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
RESEARCH = ROOT.parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bytes', type=int, default=16)
    args = parser.parse_args()
    if not 1 <= args.bytes <= 131072:
        parser.error('pattern length must be1..131072 bytes')
    owner = RESEARCH / 'build/phone.lock/owner'

    def lock():
        if not owner.exists() or owner.read_text().split()[0] != os.environ.get('CELLULAR_SESSION', 'codex-cellular'):
            raise RuntimeError('cellular must hold the phone lock before any deployment or test')

    lock()
    subprocess.run([sys.executable, str(Path(__file__).with_name('battery-gate.py')),
                    '--require-ready'], check=True)
    binary = ROOT / 'out/cellular/live/live-spi'
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    subprocess.run(['codesign', '--verify', '--strict', str(binary)], check=True)
    ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', 's20']
    probe = subprocess.run(ssh + ['/var/root/rwsys kern.redwood.poke "spi8.rx 0"'],
                           capture_output=True, text=True, timeout=10, check=True)
    if probe.stdout.strip() != 'spi8: invalid byte count/hex':
        raise RuntimeError('SPI8 kernel hooks unavailable; no hardware changes or upload')
    lock()
    out = ROOT / 'logs/cellular' / ('live-spi-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    out.mkdir(parents=True)
    target = '/var/root/cellular-live-spi-' + digest[:12]
    subprocess.run(['scp', '-q', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', str(binary),
                    's20:' + target], check=True, timeout=30)
    lock()
    subprocess.run(ssh + ['chmod 700 ' + shlex.quote(target)], check=True, timeout=10)
    lock()
    result = dict(mode='internal-loopback', pattern_bytes=args.bytes, binary_sha256=digest,
                  reboot_requested=False, cp_power_requested=False, status='started')
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Internal loopback; generated data only. Evidence:', out, flush=True)
    try:
        response = subprocess.run(ssh + ['exec ' + shlex.quote(target) + ' ' + str(args.bytes)],
                                  capture_output=True, timeout=45)
    except subprocess.TimeoutExpired as error:
        (out / 'transcript.txt').write_bytes((error.stdout or b'') + (error.stderr or b''))
        result.update(status='timeout; remote state unknown; do not repeat')
        (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print('Probe timed out; inspect phase transcript before further action.', flush=True)
        return 2
    transcript = response.stdout + response.stderr
    (out / 'transcript.txt').write_bytes(transcript)
    print(transcript.decode(errors='replace'), end='', flush=True)
    safe = b'cleanup_verified 1' in transcript
    passed = response.returncode == 0 and b'loopback_pass 1' in transcript and safe
    result.update(exit_code=response.returncode, cleanup_confirmed=safe,
                  status='complete' if passed else 'failed; inspect transcript')
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if safe:
        lock()
        subprocess.run(ssh + ['rm -f ' + shlex.quote(target)], check=True, timeout=10)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
