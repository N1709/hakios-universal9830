#!/usr/bin/env python3
"""Read-only SPI16 clock/USI baseline from documented PERIC1 CMU/SYSREG.

Default prints a plan. --read requires the phone lock. Thirteen single-word reads
only: never access the potentially unclocked SPI/PCIe controller, write a
clock/pin/reset, start CP, or transfer firmware. Rates are source-based
estimates; they are not measurements of SCLK.
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
REGISTERS = {
    'mux': 0x10700690,
    'divider': 0x10701820,
    'reset_sync_gate': 0x107020fc,
    'ipclk6_gate': 0x1070208c,
    'qchannel': 0x10703070,
    'usi_mode': 0x10721048,
    'gpp8_con': 0x10730060,
    'gpp8_dat': 0x10730064,
    'gpp8_pud': 0x10730068,
    'gpg1_con': 0x107300c0,
    'gpg1_dat': 0x107300c4,
    'gpg1_pud': 0x107300c8,
    'pcie1_phy_control': 0x15860720,
}
OSC_HZ = 26_000_000
BOOT_MAX_SCLK_HZ = 2_150_000


def command(address):
    if address not in REGISTERS.values():
        raise ValueError('address is not a reviewed CMU/SYSREG register')
    return '/var/root/rwsys kern.redwood.poke ' + shlex.quote(f'r {address:#x} 1')


def word(address, response):
    match = re.fullmatch(rf'{address:#x}: ([0-9a-f]{{8}})', response.strip(), re.I)
    if not match:
        raise ValueError('unexpected single-word debug response')
    return int(match[1], 16)


def safe_oscillator_plan():
    divisor = (OSC_HZ + 4 * BOOT_MAX_SCLK_HZ - 1) // (4 * BOOT_MAX_SCLK_HZ)
    return dict(source='26-MHz oscillator, source-derived plan only', divisor=divisor,
                divider_register=divisor-1, ip_hz=OSC_HZ/divisor,
                sclk_hz=OSC_HZ/(4*divisor), maximum_sclk_hz=BOOT_MAX_SCLK_HZ)


def decode(values):
    if set(values) != set(REGISTERS) or any(not 0 <= x <= 0xffffffff for x in values.values()):
        raise ValueError('incomplete or invalid register snapshot')
    selection = (values['mux'] >> 4) & 1
    mux_busy, divider_busy = (values['mux'] >> 16) & 1, (values['divider'] >> 16) & 1
    divisor = (values['divider'] & 15) + 1
    report = dict(usi_mode={1: 'uart', 2: 'spi', 4: 'i2c'}.get(values['usi_mode'], 'unconfigured/other'),
                  source_select=selection,
                  source='CMU_TOP PERIC1 IP (rate not measured)' if selection else 'OSCCLK_PERIC1 (26 MHz in source)',
                  mux_busy=mux_busy, divisor=divisor, divider_busy=divider_busy,
                  qchannel=hex(values['qchannel']), gate_fields={},
                  spi_controller_access_verified=False,
                  scope='read-only clock-control state; no CP execution or SCLK measurement implied')
    report['pins'] = {
        f'gpp8-{pin}': dict(function=(values['gpp8_con'] >> (4*pin)) & 15,
                            level=(values['gpp8_dat'] >> pin) & 1,
                            pull=(values['gpp8_pud'] >> (4*pin)) & 15,
                            shared_with_dump_notification=(pin == 6))
        for pin in range(4, 8)
    }
    report['ap2cp_wakeup'] = dict(pin='gpg1-3', function=(values['gpg1_con'] >> 12) & 15,
                                   level=(values['gpg1_dat'] >> 3) & 1,
                                   pull=(values['gpg1_pud'] >> 12) & 15)
    report['pcie1_phy_bypass'] = values['pcie1_phy_control'] & 1
    report['pcie1_phy_isolation_asserted'] = not bool(values['pcie1_phy_control'] & 1)
    for name in ('reset_sync_gate', 'ipclk6_gate'):
        value = values[name]
        report['gate_fields'][name] = dict(cg_val=(value >> 21) & 1,
                                          manual=(value >> 20) & 1, automatic=(value >> 28) & 1)
    if not selection and not mux_busy and not divider_busy:
        report['estimated_ip_hz'] = OSC_HZ/divisor
        report['estimated_sclk_hz'] = OSC_HZ/(4*divisor)
        report['estimated_within_boot_limit'] = OSC_HZ <= 4*divisor*BOOT_MAX_SCLK_HZ
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--read', action='store_true')
    parser.add_argument('--host', default='s20')
    parser.add_argument('--session', default='codex-cellular')
    args = parser.parse_args()
    if args.host.startswith('-'):
        parser.error('invalid SSH host')
    plan = {name: dict(address=hex(address), command=command(address)) for name, address in REGISTERS.items()}
    if not args.read:
        print(json.dumps(dict(mode='plan only; no phone access', reads=plan,
                              conservative_clock_plan=safe_oscillator_plan()), indent=2))
        return
    owner_file = RESEARCH / 'build/phone.lock/owner'
    def require_lock():
        if not owner_file.exists() or owner_file.read_text().split()[0] != args.session:
            raise RuntimeError('No hardware reads: acquire the normal phone lock first')
    require_lock()
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    out = ROOT / 'logs/cellular' / ('clocks-' + stamp)
    out.mkdir(parents=True)
    result = dict(complete=False, observations={}, collected_at=stamp)
    ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=3', args.host]
    for name, address in REGISTERS.items():
        require_lock()
        response = subprocess.run(ssh + [command(address)], capture_output=True, text=True, timeout=12, check=True)
        result['observations'][name] = word(address, response.stdout)
        (out / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    result['decoded'] = decode(result['observations'])
    result['complete'] = True
    (out / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result['decoded'], indent=2))
    print(out / 'report.json')


if __name__ == '__main__':
    main()
