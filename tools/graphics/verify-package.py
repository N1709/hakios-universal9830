#!/usr/bin/env python3
"""Check S20 recovery packing against the loader's fixed read windows."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('directory', type=Path)
a = p.parse_args()
d = a.directory
manifest = json.loads((d / 'fixed-manifest.json').read_text())
image = (d / 'fixed-recovery.img').read_bytes()
kernel_size = manifest['kernel_bytes']
assert 0 < kernel_size <= 0x1000000, 'kernel exceeds loader 16 MiB read window; strip local symbols'
kernel = image[0x2000000:0x2000000 + kernel_size]
assert kernel[:4] == bytes.fromhex('cffaedfe'), 'kernel Mach-O missing'
assert hashlib.sha256(kernel).hexdigest() == manifest['kernel_sha256']
root = image[0x3400000:0x3400000 + manifest['root_bytes']]
assert hashlib.sha256(root).hexdigest() == manifest['root_sha256']
assert len(image) == 69009408, 'unexpected recovery partition size'
assert image[0x201800:0x201804] == bytes.fromhex('d7b7ab1e'), 'Samsung DT table missing'
i = 0x100870 + 8
root_size = None
while True:
    name = image[i:i + 16].rstrip(b'\0')
    size = struct.unpack_from('<I', image, i + 16)[0]
    assert i + 24 + size <= 0x180870, 'payload record outside payload window'
    if not name:
        break
    if name == b'rsize':
        root_size = int(image[i + 24:i + 24 + size].rstrip(b'\0'), 16)
    i += 24 + (size + 15) // 16 * 16
assert root_size == len(root), 'loader root size mismatch'
assert (d / 'fixed-head.bin').read_bytes() == image[:0x1000000]
assert (d / 'fixed-tail.bin').read_bytes() == image[0x2000000:]
assert (d / 'fixed-boot.img').read_bytes() == image[:manifest['boot_bytes']]
for flag in (b'softpasscode=1', b'autounlock=0'):
    assert flag in image[:0x1000000], 'authentication boot flag missing'
print('PASS: loader windows, kernel/root hashes, payload size, DT table, flash chunks and authentication flags')
