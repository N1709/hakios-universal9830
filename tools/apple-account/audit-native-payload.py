#!/usr/bin/env python3
"""Offline audit of the reviewed iOS 16.5 akd binary; no device or account access.

Requires capstone. Addresses are deliberately pinned to one binary hash. This
reports static call sites and constants, not captured authentication requests.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

REVIEWED_SHA256 = 'a99db1b0090cab40900a3ad2576379969ffd2e2c75b2b333833367cc4efb8473'


class Image:
    def __init__(self, data):
        if hashlib.sha256(data).hexdigest() != REVIEWED_SHA256:
            raise ValueError('Unsupported binary; reviewed addresses cannot be reused')
        self.data = data
        self.segments = []
        offset = 32
        for _ in range(struct.unpack_from('<I', data, 16)[0]):
            command, size = struct.unpack_from('<II', data, offset)
            if command == 0x19:
                vm, _, file_offset, file_size = struct.unpack_from('<QQQQ', data, offset + 24)
                self.segments.append((vm, file_offset, file_size))
            offset += size
        self.decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)

    def read(self, address, size):
        for vm, offset, length in self.segments:
            if vm <= address and address + size <= vm + length:
                return self.data[offset + address - vm:offset + address - vm + size]
        raise ValueError('Address outside file-backed segments')

    def pointer(self, address):
        value = struct.unpack('<Q', self.read(address, 8))[0]
        # Reviewed image uses image-relative chained rebases for these fields.
        return 0x100000000 + (value & 0xfffffffff) if value >> 40 else value

    def string(self, address):
        return self.read(address, 160).split(b'\0', 1)[0].decode('ascii')

    def selector(self, address):
        instructions = list(self.decoder.disasm(self.read(address, 8), address))
        if (len(instructions) != 2 or instructions[0].mnemonic != 'adrp'
                or not instructions[0].op_str.startswith('x1,')
                or instructions[1].mnemonic != 'ldr'
                or not instructions[1].op_str.startswith('x1, [x1,')):
            raise ValueError('Unexpected selector stub')
        page = int(instructions[0].op_str.split('#')[1], 0)
        offset = int(instructions[1].op_str.split('#')[1].rstrip(']'), 0)
        return self.string(self.pointer(page + offset))

    def call(self, address):
        instruction = next(self.decoder.disasm(self.read(address, 4), address))
        if instruction.mnemonic != 'bl':
            raise ValueError('Expected direct call')
        return {'address': hex(address),
                'selector': self.selector(int(instruction.op_str.lstrip('#'), 0))}

    def constant(self, address):
        return self.string(self.pointer(address + 16))


def audit(image):
    call_sites = [0x10008d2c8, 0x10008d2d4, 0x10008d4fc,
                  0x10008d538, 0x10008d560, 0x10008d588,
                  0x10008de3c, 0x10008de50]
    constants = [0x100124a98, 0x100124ab8, 0x100124ad8,
                 0x100124af8, 0x100121a38]
    return {
        'binary_sha256': REVIEWED_SHA256,
        'evidence': 'static instructions and constants, not a live request',
        'method': 'AKSRPContextHelper.authenticationParametersWithCompletion:',
        'method_address': '0x10008cfe4',
        'anisette_completion_address': '0x10008ddd0',
        'calls': [image.call(address) for address in call_sites],
        'constants': [{'address': hex(address), 'key': image.constant(address)}
                      for address in constants],
        'live_server_acceptance_tested': False,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    args = parser.parse_args()
    try:
        result = audit(Image(args.binary.read_bytes()))
    except (OSError, ValueError, struct.error) as error:
        parser.exit(1, f'Audit refused: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
