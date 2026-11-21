#!/usr/bin/env python3
"""Audit one bounded CP BOOT preflight report, offline. Never proves LTE/CP acceptance."""
import argparse
import json
from pathlib import Path
import re


def audit(report, manifest):
    expected = manifest.get('cp_boot', {}).get('size')
    if type(expected) is not int or not 1 <= expected <= 131072:
        raise ValueError('missing or invalid BOOT length in manifest')
    if not (manifest.get('installed_firmware_preserved') and
            manifest.get('installed_loader_fallback') and manifest.get('avb_verified')):
        raise ValueError('not a verified original-firmware CP probe manifest')
    if report.count('== CELLULAR CP BOOT PREFLIGHT') != 1:
        raise ValueError('expected exactly one CP probe report')
    errors = []
    required = (
        'cellular memory: reserved 3/3',
        'cellular memory: normal-nc 3/3',
        'cellular MMIO: preflight result 1',
        'cellular SPI loopback: preflight result 1',
        'cellular GPIO: preflight result 1',
        'cellular CP BOOT: SPI pad ownership verified 1',
        'cellular CP BOOT: SPI/clock/pad/AP ownership restored 1; power requested 1',
        'cellular preflight: handing off to the original loader',
    )
    for marker in required:
        if not any(line.startswith(marker) for line in report.splitlines()):
            errors.append('missing: ' + marker)
    transfers = re.findall(r'^cellular CP BOOT: PIO error (-?\d+) queued (\d+) completed (\d+) packets (\d+) status ([0-9a-fA-F]+)$', report, re.M)
    summaries = re.findall(r'^cellular CP BOOT: error (-?\d+) phase (\d+) completed (\d+) cleanup safe ([01]); CP acceptance unproven$', report, re.M)
    if len(transfers) != 1 or len(summaries) != 1:
        errors.append('expected exactly one transfer and one final summary')
    else:
        error, queued, completed, packets = map(int, transfers[0][:4])
        final_error, phase, final_completed, safe = map(int, summaries[0])
        if error or queued != expected or completed != expected or packets != (expected + 63) // 64:
            errors.append('SPI transfer did not complete the exact BOOT length')
        if final_error or phase != 6 or final_completed != expected or safe != 1:
            errors.append('power/transfer/shutdown lifecycle did not pass')
    if 'cellular CP BOOT: HOLD;' in report:
        errors.append('unsafe-cleanup hold occurred; review recovery separately')
    return dict(controller_transfer_and_shutdown_pass=not errors, errors=errors,
                expected_bytes=expected, cp_acceptance='unproven',
                sim_detection='unproven', lte='unproven', volte='unproven',
                boot_partition_restored='requires independent partition readback')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = audit(args.report.read_text(), json.loads(args.manifest.read_text()))
    except (ValueError, KeyError) as error:
        parser.exit(2, str(error) + '\n')
    print(json.dumps(result, indent=2))
    return 0 if result['controller_transfer_and_shutdown_pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
