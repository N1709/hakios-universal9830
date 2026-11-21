#!/usr/bin/env python3
"""Compare reconstructed CCM9 maths with one isolated AArch64 helper in Unicorn.

Hash-pinned stock routine 0x2b4938..0x2b4bdc only. Read-only library mapping,
one executable page, scratch/stack mappings, control-flow guard, instruction
limit. No host/device calls, loader, HAL, MMIO or firmware initialization.
Requires unicorn. Firmware/derived values stay local; output is counts only.
"""
import argparse
import hashlib
import importlib.util
import random
import struct
from pathlib import Path
from samsung_ccm_math import ccm9_to_matrices

HERE = Path(__file__).resolve().parent
DDK_SHA256 = 'e047faff00230a58dd547c6fe0503d91223f3a888730dd550b2dedbedf7d99b5'


def verifier(data):
    if hashlib.sha256(data).hexdigest() != DDK_SHA256:
        raise ValueError('unsupported DDK hash')
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_READ, UC_PROT_EXEC, UC_PROT_WRITE, UC_HOOK_CODE
    from unicorn.arm64_const import (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
                                    UC_ARM64_REG_X3, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_PC)
    machine = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    base, scratch, stop = 0x1000000, 0x2000000, 0x3000000
    machine.mem_map(base, (len(data) + 4095) & ~4095, UC_PROT_READ)
    machine.mem_write(base, data)
    machine.mem_protect(base + 0x2b4000, 4096, UC_PROT_READ | UC_PROT_EXEC)
    machine.mem_map(scratch, 0x10000, UC_PROT_READ | UC_PROT_WRITE)

    def guard(uc, pc, size, context):
        if not base + 0x2b4938 <= pc < base + 0x2b4bdc:
            raise ValueError(f'CCM routine left allowed control-flow region: {pc - base:#x}')

    machine.hook_add(UC_HOOK_CODE, guard)

    def check(vectors, pre):
        machine.mem_write(scratch + 0x1000, struct.pack('<27h', *vectors))
        machine.mem_write(scratch + 0x2000, struct.pack('<9h', *pre))
        machine.mem_write(scratch + 0x3000, bytes(108))
        for reg, value in ((UC_ARM64_REG_X0, 0), (UC_ARM64_REG_X1, scratch + 0x3000),
                           (UC_ARM64_REG_X2, scratch + 0x1000), (UC_ARM64_REG_X3, scratch + 0x2000),
                           (UC_ARM64_REG_LR, stop), (UC_ARM64_REG_SP, scratch + 0xf000)):
            machine.reg_write(reg, value)
        machine.emu_start(base + 0x2b4938, stop, count=30000)
        if machine.reg_read(UC_ARM64_REG_PC) != stop:
            raise ValueError('CCM routine did not return within instruction budget')
        actual = list(struct.unpack('<54h', bytes(machine.mem_read(scratch + 0x3000, 108))))
        expected = [v for sector in ccm9_to_matrices(vectors, pre) for v in sector]
        if actual != expected:
            first = next(i for i, (a, b) in enumerate(zip(actual, expected)) if a != b)
            raise ValueError(f'CCM reconstruction differs at coefficient {first}')
    return check


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ddk', type=Path)
    parser.add_argument('setfiles', nargs='*', type=Path)
    args = parser.parse_args()
    try:
        check = verifier(args.ddk.read_bytes())
        rng = random.Random(20)
        for _ in range(200):
            check([rng.randint(-32768, 32767) for _ in range(27)],
                  [rng.randint(-32768, 32767) for _ in range(9)])
        print('200 deterministic full-i16-range synthetic cases matched', flush=True)
        spec = importlib.util.spec_from_file_location('tuning', HERE / 'inspect-samsung-tuning.py')
        tuning = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tuning)
        schema = tuning.load_schema()
        ccm_block = next(b for b in schema['blocks'] if b['key'] == 'ccmTuning')
        for path in args.setfiles:
            data = path.read_bytes()
            metadata = tuning.container.inspect(data)
            group = next(g for g in metadata['groups'] if g['design'] == '3AA_ISP')
            count = 0
            for entry in group['entries']:
                blocks = tuning.split_payload(data[entry['offset']:entry['offset'] + entry['size']], schema)
                ccm = blocks[ccm_block['key']]
                for i in range(34):
                    at = 0x78 + i * 0xf4
                    pre = struct.unpack_from('<9h', ccm, at)
                    for offset in (0x24, 0x5a, 0xba):
                        check(struct.unpack_from('<27h', ccm, at + offset), pre)
                        count += 1
            print(f'{path.name}: {count} stock vector/matrix cases matched', flush=True)
    except (OSError, ValueError, ImportError, struct.error, StopIteration) as error:
        parser.exit(1, f'CCM verification: {error}\n')


if __name__ == '__main__':
    main()
