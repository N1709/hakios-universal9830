#!/usr/bin/env python3
"""Recover key order/sizes by bounded symbolic tracing of one known DDK dispatch.

Requires capstone. Only the allowlisted initializer at 0x1523cc..0x152c60 is
interpreted; strcmp is modeled using file strings, memset is a no-op on our fresh
symbolic state. No firmware code, callbacks, allocations or device I/O execute.
Unknown binaries/instructions/calls fail. Output is layout metadata, no payloads.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

DDK_SHA256 = 'e047faff00230a58dd547c6fe0503d91223f3a888730dd550b2dedbedf7d99b5'


def recover(data):
    if hashlib.sha256(data).hexdigest() != DDK_SHA256:
        raise ValueError('DDK hash differs from researched binary; offsets are not portable')
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_REG

    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    decoder.detail = True
    instructions = {i.address: i for i in decoder.disasm(data[0x1523cc:0x152c60], 0x1523cc)}
    relocation = struct.unpack_from('<Q', data, 0x35f030)[0] - 0x357e40
    registers = {29: 0x1000000, 20: 0x2000000}
    memory, sizes = {}, {}
    pc, equal = 0x1523cc, False

    def regno(reg):
        name = decoder.reg_name(reg)
        aliases = {'fp': 29, 'lr': 30, 'sp': 31, 'xzr': 31, 'wzr': 31}
        return aliases[name] if name in aliases else int(name[1:])

    def read(operand):
        if operand.type == ARM64_OP_IMM:
            return operand.imm
        if operand.type != ARM64_OP_REG:
            raise ValueError('unsupported symbolic operand')
        mask = 0xffffffff if decoder.reg_name(operand.reg).startswith('w') else 0xffffffffffffffff
        return registers.get(regno(operand.reg), 0) & mask

    def put(operand, value):
        mask = 0xffffffff if decoder.reg_name(operand.reg).startswith('w') else 0xffffffffffffffff
        registers[regno(operand.reg)] = value & mask

    def address(operand):
        value = registers.get(regno(operand.mem.base), 0) + operand.mem.disp
        if operand.mem.index:
            value += registers.get(regno(operand.mem.index), 0) << operand.shift.value
        return value

    def string(at):
        if not 0 <= at < len(data):
            raise ValueError('string outside DDK')
        end = data.find(b'\0', at, at + 256)
        if end < 0:
            raise ValueError('unterminated DDK key')
        return data[at:end].decode('ascii')

    for _ in range(30000):
        if pc == 0x1524c4:
            break
        instruction = instructions.get(pc)
        if instruction is None:
            raise ValueError(f'control flow outside allowlisted dispatch: {pc:#x}')
        op, name, next_pc = instruction.operands, instruction.mnemonic, pc + 4
        if name in ('mov', 'adrp'):
            put(op[0], read(op[1]))
        elif name == 'movk':
            shift = op[1].shift.value
            put(op[0], (read(op[0]) & ~(65535 << shift)) | (read(op[1]) << shift))
        elif name == 'add':
            if op[2].shift.value:
                raise ValueError('unexpected shifted ADD in dispatch')
            put(op[0], read(op[1]) + read(op[2]))
        elif name == 'str':
            at = address(op[1])
            if not (0x1000000 <= at < 0x1000100 or 0x2000000 <= at < 0x2000104):
                raise ValueError('write outside symbolic stack/size table')
            memory[at] = read(op[0])
            if 0x2000000 <= at < 0x2000104:
                sizes[(at - 0x2000000) // 4] = read(op[0])
        elif name == 'ldr':
            at = address(op[1])
            value = memory.get(at)
            if value is None:
                if not 0x35f030 <= at < 0x35f030 + 65 * 8:
                    raise ValueError('unexpected DDK read outside key-pointer table')
                value = struct.unpack_from('<Q', data, at)[0] - relocation
            put(op[0], value)
        elif name == 'cmp':
            equal = read(op[0]) == read(op[1])
        elif name in ('b.eq', 'b.ne'):
            if equal == (name == 'b.eq'):
                next_pc = read(op[0])
        elif name == 'b':
            next_pc = read(op[0])
        elif name in ('cbz', 'cbnz'):
            if (read(op[0]) == 0) == (name == 'cbz'):
                next_pc = read(op[1])
        elif name == 'bl':
            if read(op[0]) == 0xe3944:
                registers[0] = int(string(registers[0]) != string(registers[1]))
            elif read(op[0]) != 0xe1244:
                raise ValueError(f'unknown call in dispatch: {instruction.op_str}')
        else:
            raise ValueError(f'unknown instruction: {instruction.mnemonic} {instruction.op_str}')
        pc = next_pc
    else:
        raise ValueError('symbolic step limit exceeded')
    if set(sizes) != set(range(65)):
        raise ValueError('dispatch did not assign every key size')
    blocks, offset = [], 8
    for i in range(65):
        pointer = struct.unpack_from('<Q', data, 0x35f030 + i * 8)[0] - relocation
        blocks.append(dict(key=string(pointer), size=sizes[i], offset=offset))
        offset += sizes[i]
    return dict(ddk_sha256=DDK_SHA256, key_table_offset='0x35f030',
                size_dispatch_offset='0x1523cc', payload_magic='0x3e671996',
                serialized_bytes=offset - 8, blocks=blocks)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ddk', type=Path, help='local is_lib.bin')
    args = parser.parse_args()
    try:
        result = recover(args.ddk.read_bytes())
    except (OSError, ValueError, ImportError, struct.error) as error:
        parser.exit(1, f'DDK schema: {error}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
