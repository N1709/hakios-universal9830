#!/usr/bin/env python3
"""Synthetic DTB tests; no copied Samsung DTB or biometric data in the repo."""
from pathlib import Path
import runpy
import struct
import tempfile

inventory=runpy.run_path(str(Path(__file__).with_name('inventory.py')))['inventory']
u=lambda n:struct.pack('>I',n)
strings=b'compatible\0qcom,ipc-gpio\0private-template\0'
def pad(b):return b+bytes(-len(b)%4)
def node(name):return u(1)+pad(name.encode()+b'\0')
def prop(key,value):return u(3)+u(len(value))+u(strings.index(key.encode()+b'\0'))+pad(value)
def blob(compat,cells):
    tree=(node('')+node('qbt2000')+prop('compatible',compat+b'\0')+
          prop('qcom,ipc-gpio',cells)+prop('private-template',b'must-not-be-reported')+
          u(2)+u(2)+u(9))
    offset=56
    return (struct.pack('>10I',0xd00dfeed,offset+len(tree)+len(strings),offset,offset+len(tree),40,
                        17,16,0,len(strings),len(tree))+bytes(16)+tree+strings)
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp)/'fixture.dtb'
    p.write_bytes(blob(b'qcom,qbt2000',u(0xffffffff)+u(3)+u(0)))
    r=inventory(p);n=r['fingerprint_nodes'][0]
    assert n['properties']['qcom,ipc-gpio']['unresolved_overlay_fixup']
    assert 'private-template' not in n['properties']
    p.write_bytes(blob(b'qcom,unrelated',u(3)))
    assert not inventory(p)['fingerprint_nodes']
    p.write_bytes(blob(b'qcom,qbt2000',b'bad'))
    try:inventory(p)
    except ValueError:pass
    else:raise AssertionError('malformed cells accepted')
print('PASS: fingerprint identification, unresolved-fixup handling, property allowlist and malformed-cell rejection')
