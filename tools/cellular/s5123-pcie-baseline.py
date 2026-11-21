#!/usr/bin/env python3
"""PCIe1 prerequisites only: read CMU/GPIO/PMU, never isolated PHY/DBI/ELBI.

Addresses from pinned Exynos9830 cmucal-sfr.c and HSI2 pinctrl. HSI2 also
serves storage: this tool NEVER writes gates, muxes, resets or pin ownership.
Default is offline plan. --read requires codex-cellular phone lock.
"""
import os
import argparse
import datetime
import importlib.util
import json
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
RESEARCH = ROOT.parent.parent
spec = importlib.util.spec_from_file_location('clock_words', Path(__file__).with_name('s5123-clocks.py'))
words = importlib.util.module_from_spec(spec)
spec.loader.exec_module(words)
REGISTERS = {
    'phy_isolation': 0x15860720,
    'gpf3_con': 0x13c30000, 'gpf3_dat': 0x13c30004, 'gpf3_pull': 0x13c30008,
    'bus_mux': 0x13c00600, 'pcie_ref_mux': 0x13c00610,
    'dbi_gate': 0x13c02018, 'master_gate': 0x13c0201c,
    'slave_gate': 0x13c02020, 'elbi_gate': 0x13c02024,
    'ref_gate': 0x13c02028, 'pcs_gate': 0x13c0202c,
    'pma_gate': 0x13c02030, 'ia_gate': 0x13c02034,
    'qch_ref': 0x13c03000, 'qch_apb': 0x13c03028,
    'qch_axi': 0x13c0302c, 'qch_dbi': 0x13c03030,
    'qch_pcs': 0x13c03034, 'qch_pma': 0x13c03038,
    'qch_ia': 0x13c0303c,
}


def command(address):
    if address not in REGISTERS.values():
        raise ValueError('not a reviewed prerequisite register')
    return '/var/root/rwsys kern.redwood.poke ' + shlex.quote(f'r {address:#x} 1')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--read', action='store_true')
    args = parser.parse_args()
    if not args.read:
        print(json.dumps({name: hex(address) for name, address in REGISTERS.items()}, indent=2))
        return
    owner = RESEARCH / 'build/phone.lock/owner'

    def lock():
        if not owner.exists() or owner.read_text().split()[0] != os.environ.get('CELLULAR_SESSION', 'codex-cellular'):
            raise RuntimeError('cellular does not hold the phone lock')

    lock()
    out = ROOT / 'logs/cellular' / ('pcie-baseline-' + datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'))
    out.mkdir(parents=True)
    result = dict(complete=False, observations={}, scope='prerequisites only; no PCIe controller access or writes')
    for name, address in REGISTERS.items():
        lock()
        response = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=3',
                                   's20', command(address)], capture_output=True, text=True, timeout=12, check=True)
        lock()
        value = words.word(address, response.stdout)
        result['observations'][name] = value
        (out / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
        print(name, hex(value), flush=True)
    result['complete'] = True
    (out / 'report.json').write_text(json.dumps(result, indent=2) + '\n')
    print(out / 'report.json')


if __name__ == '__main__':
    main()
