#!/usr/bin/env python3
"""Read allocator preflight evidence from its bounded payload RAM entry.

Requires the phone lock. Reads only the manifest's probe.report entry inside
the UEFI FD payload, through the existing kernel debug port. No register,
partition, firmware or modem writes. Keep the output file outside git.
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import struct
import subprocess

RESEARCH = Path(__file__).resolve().parents[4]


def report_region(manifest):
    if not (manifest.get('installed_firmware_preserved') and manifest.get('installed_loader_fallback')):
        raise ValueError('not an original-firmware allocator preflight manifest')
    address, size = int(manifest['preflight_report_pa'], 16), manifest['preflight_report_size']
    if size not in (8192, 16384) or address & 3 or not 0xe8100000 <= address <= 0xe8180000 - size:
        raise ValueError('report is outside the bounded UEFI payload RAM slot')
    return address, size


def decode_words(address, count, response):
    if not 1 <= count <= 64:
        raise ValueError('read count must be 1..64 words')
    result = bytearray()
    for line in response.strip().splitlines():
        match = re.fullmatch(r'(0x[0-9a-fA-F]+):((?: [0-9a-fA-F]{8}){1,4})', line.strip())
        if not match or int(match[1], 16) != address + len(result):
            raise ValueError('unexpected debug response address or format')
        result.extend(b''.join(struct.pack('<I', int(word, 16)) for word in match[2].split()))
    if len(result) != count * 4:
        raise ValueError('short or oversized debug read')
    return bytes(result)


def collect(manifest, read):
    address, size = report_region(manifest)
    magic, length = struct.unpack('<II', read(address, 2))
    if magic != 0x304d5043 or not 0 < length <= size - 8:
        raise ValueError('no valid CPM0 preflight report; do not infer a pass')
    data = bytearray()
    while len(data) < length:
        data.extend(read(address + 8 + len(data), min(64, (length - len(data) + 3) // 4)))
    return bytes(data[:length])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--session', default='codex-cellular')
    parser.add_argument('--host', default='s20')
    args = parser.parse_args()
    if args.host.startswith('-'):
        parser.error('invalid SSH host')
    manifest = json.loads(args.manifest.read_text())
    report_region(manifest)
    owner_file = RESEARCH / 'build/phone.lock/owner'
    if not owner_file.exists() or owner_file.read_text().split()[0] != args.session:
        parser.exit(3, 'No RAM reads: acquire the normal phone lock first.\n')
    ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=3', args.host]

    def read(address, count):
        command = '/var/root/rwsys kern.redwood.poke ' + shlex.quote(f'r {address:#x} {count}')
        result = subprocess.run(ssh + [command], capture_output=True, text=True, timeout=12, check=True)
        return decode_words(address, count, result.stdout)

    data = collect(manifest, read)
    args.output.write_bytes(data)
    for line in data.decode('ascii').splitlines():
        if line.startswith(('cellular ', 'download buffer:', 'wifi dma:', 'display pool:')):
            print(line)


if __name__ == '__main__':
    main()
