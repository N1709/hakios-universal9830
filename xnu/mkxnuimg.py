#!/usr/bin/env python3
"""
Pack the redwood UEFI (with the hook that starts the XNU loader) and an optional payload
into an Android boot image for `fastboot boot`.

  kernel = gzip(BootShim + UEFI FD + ["XNUP" container]) + DTB

The boot shim copies the FD to its base and the payload to PAYLOAD_BASE.

The UEFI pieces are taken from a Mu-Silicium tree built here (Mu-Silicium/, see uefi/README.md)
or, when there is none, from prebuilt/uefi/.

usage: mkxnuimg.py [-p payload.bin] [-o out.img]

Another device (common/devices/<name>.conf, read by common/build.sh) sets these in the environment:
  UEFI_TARGET   Mu-Silicium device name (default redwood)    UEFI_FD, DTB   files to use instead
  FD_BASE, FD_SIZE, PAYLOAD_BASE                              addresses from its UEFI port's memory map
"""
import argparse, gzip, os, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MU = os.path.join(HERE, 'Mu-Silicium')
PREBUILT = os.path.join(HERE, 'prebuilt', 'uefi')
def env_int(name, default):
    v = os.environ.get(name, '')
    return int(v, 0) if v else default
TARGET = os.environ.get('UEFI_TARGET') or 'redwood'
FD_BASE, FD_SIZE = env_int('FD_BASE', 0x9FD00000), env_int('FD_SIZE', 0x200000)
PAYLOAD_BASE, PAYLOAD_MAX = env_int('PAYLOAD_BASE', 0xF0000000), 0x04000000

ap = argparse.ArgumentParser()
ap.add_argument('-p', '--payload', help='XNUP payload appended after the FD (copied to PAYLOAD_BASE)')
ap.add_argument('--fd-payload', help='XNUP payload spliced into the FD tail (for the UEFI XnuLoader hook)')
ap.add_argument('--ramdisk', help='ramdisk file (default: the UEFI tree ramdisk, or /dev/null)')
ap.add_argument('-o', '--output', default=os.path.join(HERE, 'xnu-redwood.img'))
ap.add_argument('--mode', default='RELEASE')
args = ap.parse_args()

fd_path = os.environ.get('UEFI_FD') or os.path.join(MU, f'Build/{TARGET}Pkg/{args.mode}_CLANGPDB/FV/SILICIUM_UEFI.fd')
if os.path.isfile(fd_path) and os.path.isdir(os.path.join(MU, 'BootShim')):
    shim_dir = os.path.join(MU, 'BootShim')
    for f in ('BootShim.elf', 'BootShim.bin'):
        try: os.unlink(os.path.join(shim_dir, f))
        except FileNotFoundError: pass
    subprocess.check_call(['make', 'REQUIRES_KERNEL_HEADER=1', f'FD_BASE={FD_BASE:#x}', f'FD_SIZE={FD_SIZE:#x}',
                           f'CFLAGS=--target=aarch64-linux-gnu -DPAYLOAD_BASE={PAYLOAD_BASE:#x}'],
                          cwd=shim_dir, stdout=subprocess.DEVNULL)
    parts = {'shim': os.path.join(shim_dir, 'BootShim.bin'), 'fd': fd_path,
             'dtb': os.path.join(MU, f'Resources/DTBs/{TARGET}.dtb'), 'ramdisk': os.path.join(MU, 'Resources/ramdisk'),
             'mkbootimg': os.path.join(MU, 'Resources/Scripts/mkbootimg.py')}
else:
    parts = {'shim': os.path.join(PREBUILT, 'BootShim.bin'), 'fd': os.path.join(PREBUILT, 'SILICIUM_UEFI.fd'),
             'dtb': os.path.join(PREBUILT, 'redwood.dtb'), 'ramdisk': os.path.join(PREBUILT, 'ramdisk'),
             'mkbootimg': os.path.join(PREBUILT, 'mkbootimg.py')}
    if os.environ.get('UEFI_FD'): parts['fd'] = os.environ['UEFI_FD']
shim_path = os.environ.get('SHIM') or parts['shim']
mkbootimg_path = os.environ.get('MKBOOTIMG') or parts['mkbootimg']
ramdisk_path = args.ramdisk or os.environ.get('RAMDISK') or parts.get('ramdisk') or '/dev/null'
dtb_path = os.environ.get('DTB') or parts['dtb']

shim = open(shim_path, 'rb').read()
fd = bytearray(open(parts['fd'], 'rb').read())
assert len(fd) == FD_SIZE, hex(len(fd))

if args.fd_payload:
    data = open(args.fd_payload, 'rb').read()
    at = env_int('FD_PAYLOAD_AT', FD_SIZE // 2)
    limit = FD_SIZE - at
    assert 0 <= at and limit > 0, hex(at)
    # The FD tail is erased (0xff) in a normal build. Put the small
    # loader.efi container there for the UEFI XnuLoader hook.
    assert data[:4] == b'XNUP' and len(data) <= limit, 'bad or oversized FD payload'
    fd[at:at + limit] = b'\xff' * limit
    fd[at:at + len(data)] = data
    print(f'fd payload: {len(data)} bytes at FD+{at:#x}, bytesum={sum(data) & 0xffffffff:08x}')

blob = shim + bytes(fd)
if args.payload:
    data = open(args.payload, 'rb').read()
    # the payload file is a complete "XNUP" container (see xnuloader/mkpayload.py)
    assert data[:4] == b'XNUP' and len(data) <= PAYLOAD_MAX - 0x1000, 'bad or oversized payload'
    blob += data
    blob += b'\0' * (-len(blob) % 16)
    print(f'payload: {len(data)} bytes, bytesum={sum(data) & 0xffffffff:08x}')

dtb = open(dtb_path, 'rb').read()
# Samsung's bootloader (unlike fastboot on Qualcomm) wants an uncompressed kernel and header v2,
# with the dtb in the header -- that is what the prebuilt Mu-x1s.img that boots on the S20 does.
no_gzip = os.environ.get('NOGZIP') == '1'
hdr = int(os.environ.get('HEADER_VERSION', '1'))
kernel = args.output + '.kernel~'
# mtime=0 keeps the image byte-identical across builds (and CI artifacts).
kdata = blob if no_gzip else gzip.compress(blob, 6, mtime=0)
extra = []
if hdr >= 2:
    extra = ['--dtb', dtb_path]
else:
    kdata += dtb
open(kernel, 'wb').write(kdata)
try:
    subprocess.check_call([sys.executable, mkbootimg_path, '--kernel', kernel, '--ramdisk', ramdisk_path,
                           '--header_version', str(hdr), *extra, '-o', args.output])
finally:
    os.unlink(kernel)
print(f'{args.output}: {os.path.getsize(args.output)} bytes (shim {len(shim)}, uncompressed {len(blob)})')
