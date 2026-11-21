#!/usr/bin/env python3
"""Exercise the restore helper's real read-only parser in a disposable directory.
Host tests substitute the fixture path/owner; never open the phone's keybag.
"""
from pathlib import Path
import os
import plistlib
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='swkb-files-') as tmp:
    work = Path(tmp)
    bags = work / 'keybags'
    bags.mkdir(mode=0o700)
    src = (root / 'userland/src/software_keybag_init.c').read_text()
    src = src.replace('"/private/var/keybags"', '"' + str(bags) + '"')
    src = src.replace('st_uid!=0', 'st_uid!=getuid()')
    (work / 'helper.c').write_text(src)
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror', str(work / 'helper.c'),
                    '-framework', 'CoreFoundation', '-framework', 'IOKit', '-o', str(work / 'helper')], check=True)
    def check(ok):
        r = subprocess.run([str(work / 'helper'), '--check'], capture_output=True)
        assert (r.returncode == 0) == ok, r.stderr.decode()
    check(True)  # absent before first passcode
    bag = bags / 'systembag.kb'
    bag.write_bytes(plistlib.dumps({'KeyBagKeys': b'RW-SWKB1' + bytes(80)}))
    bag.chmod(0o600)
    check(True)
    bag.chmod(0o644)
    check(False)
    bag.chmod(0o600)
    bags.chmod(0o755)
    check(False)
    bags.chmod(0o700)
    link = bags / 'extra-link'
    os.link(bag, link)
    check(False)
    link.unlink()
    bag.rename(link)
    bag.symlink_to(link)
    check(False)
    bag.unlink()
    link.rename(bag)
    bag.unlink()
    partial = bags / 'systembag.kb.writing'
    partial.write_bytes(b'partial')
    check(False)
    partial.unlink()
    os.mkfifo(bag, 0o600)
    check(False)  # O_NONBLOCK must prevent a startup hang
    bag.unlink()
    bag.write_bytes(b'not a plist')
    bag.chmod(0o600)
    check(False)
    real = work / 'real'
    bags.rename(real)
    bags.symlink_to(real, target_is_directory=True)
    check(False)
print('PASS: private-file restore, unsafe modes, hardlinks, symlinks, partial writes, FIFO and malformed data')
