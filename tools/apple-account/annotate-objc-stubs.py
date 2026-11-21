#!/usr/bin/env python3
"""Annotate arm64 Mach-O branch stubs using dyld_info -fixups rebase targets.
Reads static binaries/disassembly only; never reads process or account memory.
"""
import argparse
from pathlib import Path
import re
import struct
import capstone

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    for name in ('binary','fixups','disassembly','output'):ap.add_argument('--'+name,required=True,type=Path)
    ap.add_argument('--metadata',type=Path,help='Optional otool -ov output for method entry names')
    args=ap.parse_args();blob=args.binary.read_bytes()
    if blob[:4]!=b'\xcf\xfa\xed\xfe':raise ValueError('Expected thin little-endian Mach-O64')
    ncmds=struct.unpack_from('<I',blob,16)[0];pos=32;segments=[]
    for _ in range(ncmds):
        cmd,size=struct.unpack_from('<II',blob,pos)
        if size<8 or pos+size>len(blob):raise ValueError('Invalid load command')
        if cmd==25:
            vm,vs,off,fs=struct.unpack_from('<QQQQ',blob,pos+24)
            segments.append((vm,vs,off,fs))
        pos+=size
    def read(address,size):
        for vm,vs,off,fs in segments:
            if vm<=address and address+size<=vm+fs:return blob[off+address-vm:off+address-vm+size]
        return b''
    rebases={int(a,16):int(b,16) for a,b in re.findall(r'__objc_selrefs\s+(0x[0-9A-Fa-f]+)\s+rebase\s+(0x[0-9A-Fa-f]+)',args.fixups.read_text())}
    md=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_ARM);memo={}
    def selector(address):
        if address in memo:return memo[address]
        name=None;ins=list(md.disasm(read(address,8),address))
        if len(ins)==2 and ins[0].mnemonic=='adrp' and ins[0].op_str.startswith('x1, #') and ins[1].mnemonic=='ldr' and ins[1].op_str.startswith('x1, [x1'):
            page=int(ins[0].op_str.split('#')[1],0)
            match=re.search(r'#(0x[0-9a-f]+|[0-9]+)',ins[1].op_str)
            slot=page+(int(match.group(1),0) if match else 0)
            target=rebases.get(slot)
            if target:
                raw=read(target,512).split(b'\0',1)[0]
                if raw and all(32<=c<127 for c in raw):name=raw.decode('ascii')
        memo[address]=name;return name
    methods={}
    if args.metadata:
        pending=None
        for line in args.metadata.read_text().splitlines():
            match=re.search(r'^\s+name\s+0x[0-9a-f]+ \(0x([0-9a-f]+)\)',line)
            if match:
                target=rebases.get(int(match.group(1),16))
                raw=read(target,512).split(b'\0',1)[0] if target else b''
                pending=raw.decode('ascii') if raw and all(32<=c<127 for c in raw) else None
            match=re.search(r'^\s+imp\s+0x[0-9a-f]+ \(0x([0-9a-f]+)\)',line)
            if match and pending:
                methods[int(match.group(1),16)]=pending;pending=None
    count=0;output=[]
    for line in args.disassembly.read_text().splitlines():
        match=re.search(r'\b(?:bl|b)\s+(0x[0-9a-f]+)',line)
        name=selector(int(match.group(1),16)) if match else None
        entry=re.match(r'^([0-9a-f]{16})\s',line)
        if entry and int(entry.group(1),16) in methods:output.append('; METHOD '+methods[int(entry.group(1),16)])
        if name:line+=' ; objc ['+name+']';count+=1
        output.append(line)
    args.output.write_text('\n'.join(output)+'\n')
    print(f'Annotated {count} Objective-C calls from static binary')
if __name__=='__main__':main()
