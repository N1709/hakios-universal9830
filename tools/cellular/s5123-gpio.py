#!/usr/bin/env python3
"""Read only six S5123 GPIO banks in always-on CMGP, via the existing kernel.

Default prints the plan. --read requires the normal phone lock and executes
six bounded `r PA 3` debug commands: CON/DAT/PUD only, never GPIO or CP writes.
No PCIe, SPI, firmware, SIM, EFS, clock/reset registers or packet memory access.
"""
import argparse
import datetime
import json
from pathlib import Path
import re
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
RESEARCH = ROOT.parent.parent
CMGP = 0x15c30000
# x1slte revision-21 overlay + exynos9830 pinctrl bank offsets.
BANKS = (
    ('reset_n', 'gpm9-0', 0x120),
    ('power_on', 'gpm20-0', 0x280),
    ('pda_active', 'gpm25-0', 0x320),
    ('phone_active', 'gpm28-0', 0x380),
    ('cp2ap_wakeup', 'gpm30-0', 0x3c0),
    ('ps_hold', 'gpm31-0', 0x3e0),
)


def decode(address, response):
    pattern = rf'(?:0x)?{address:x}:\s+([0-9a-fA-F]{{8}})\s+([0-9a-fA-F]{{8}})\s+([0-9a-fA-F]{{8}})'
    match = re.fullmatch(pattern, response.strip(), re.I)
    if not match:
        raise ValueError('unexpected debug response; no further reads attempted')
    con, dat, pud = (int(value, 16) for value in match.groups())
    function, level, pull = con & 15, dat & 1, pud & 15
    return dict(con=hex(con), dat=hex(dat), pud=hex(pud), function=function,
                function_name={0: 'input', 1: 'output', 15: 'interrupt'}.get(function, 'alternate'),
                physical_level=level, pull=pull,
                pull_name={0: 'none', 1: 'down', 3: 'up'}.get(pull, 'other'))


def command(address):
    if address not in {CMGP + offset for _, _, offset in BANKS}:
        raise ValueError('address is not one of the six reviewed CMGP banks')
    return '/var/root/rwsys kern.redwood.poke ' + shlex.quote(f'r {address:#x} 3')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--read', action='store_true')
    parser.add_argument('--session', default='codex-cellular')
    parser.add_argument('--host', default='s20')
    args = parser.parse_args()
    if args.host.startswith('-'):
        parser.error('invalid host')
    plan = [dict(signal=name, pin=pin, address=hex(CMGP + offset),
                 command=command(CMGP + offset)) for name, pin, offset in BANKS]
    if not args.read:
        print(json.dumps(dict(mode='plan only; no phone access', reads=plan), indent=2))
        return 0
    owner_file = RESEARCH / 'build/phone.lock/owner'
    owner = owner_file.read_text().split()[0] if owner_file.exists() else None
    if owner != args.session:
        parser.exit(3, f'No hardware reads: phone lock is {owner or "free"}; acquire it as {args.session} first.\n')
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    out = ROOT / 'logs/cellular' / ('gpio-' + stamp)
    out.mkdir(parents=True)
    report = dict(scope='CMGP physical levels only; not proof of modem boot, SIM or registration',
                  collected_at=stamp, observations=[], complete=False)
    ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', args.host]
    try:
        kernel = subprocess.run(ssh + ['uname -a'], capture_output=True, text=True, timeout=12, check=True)
        report['kernel'] = kernel.stdout.strip()
        for read in plan:
            result = subprocess.run(ssh + [read['command']], capture_output=True, text=True, timeout=12, check=True)
            read['decoded'] = decode(int(read['address'], 16), result.stdout)
            report['observations'].append(read)
            print(read['signal'], read['pin'], json.dumps(read['decoded']), flush=True)
        report['complete'] = True
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report['error'] = str(error)
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(out / 'report.json')
    return 0 if report['complete'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
