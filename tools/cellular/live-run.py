#!/usr/bin/env python3
"""Deploy/run one live PCIe helper under the phone lock. Never reboot or flash.

Default --status is prerequisites only. --root-id applies the isolated CP-off
root-port probe. A timeout leaves state unknown: inspect transcript, do not retry.
"""
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
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--status', action='store_true')
    mode.add_argument('--root-id', action='store_true')
    mode.add_argument('--phy-test', action='store_true')
    args = parser.parse_args()
    owner = RESEARCH / 'build/phone.lock/owner'

    def lock():
        if not owner.exists() or owner.read_text().split()[0] != os.environ.get('CELLULAR_SESSION', 'codex-cellular'):
            raise RuntimeError('No deployment/probe: cellular must hold the normal phone lock')

    lock()
    binary = ROOT / 'out/cellular/live/live-pcie'
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    subprocess.run(['codesign', '--verify', '--strict', str(binary)], check=True)
    mutating = args.root_id or args.phy_test
    if mutating:
        subprocess.run([sys.executable, str(Path(__file__).with_name('battery-gate.py')),
                        '--require-ready'], check=True)
    lock()
    out = ROOT / 'logs/cellular' / ('live-probe-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    out.mkdir(parents=True)
    target = '/var/root/cellular-live-pcie-' + digest[:12]
    ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', 's20']
    subprocess.run(['scp', '-q', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', str(binary),
                    's20:' + target], check=True, timeout=30)
    lock()
    subprocess.run(ssh + ['chmod 700 ' + shlex.quote(target)], check=True, timeout=10)
    lock()
    selected_mode = 'phy-test' if args.phy_test else 'root-id' if args.root_id else 'status'
    result = dict(mode=selected_mode, binary_sha256=digest,
                  reboot_requested=False, flash_requested=False, status='started')
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Live diagnostic started; no restart/flash. Evidence:', out, flush=True)
    try:
        response = subprocess.run(ssh + ['exec ' + shlex.quote(target) +
                                        ' --' + selected_mode],
                                  capture_output=True, timeout=45)
    except subprocess.TimeoutExpired as error:
        (out / 'transcript.txt').write_bytes((error.stdout or b'') + (error.stderr or b''))
        result.update(status='timeout; remote state unknown; do not repeat', exit_code=None)
        (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print('Live probe timed out. Read saved phase transcript before any next action.', flush=True)
        return 2
    transcript = response.stdout + response.stderr
    (out / 'transcript.txt').write_bytes(transcript)
    print(transcript.decode(errors='replace'), end='', flush=True)
    confirmed = not mutating or b'cleanup_verified 1' in transcript
    result.update(exit_code=response.returncode, cleanup_confirmed=confirmed,
                  status='complete' if response.returncode == 0 and confirmed else 'failed; inspect transcript')
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    if confirmed:
        lock()
        subprocess.run(ssh + ['rm -f ' + shlex.quote(target)], check=True, timeout=10)
    return 0 if response.returncode == 0 and confirmed else 1


if __name__ == '__main__':
    raise SystemExit(main())
