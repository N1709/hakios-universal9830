#!/usr/bin/env python3
"""Verify a z3s Android boot image without a phone.

Checks the Android header, the gzip kernel blob, the BootShim constants, the
UEFI FD, the FD-tail XNUP payload that carries loader.efi, and the appended
XNUP payload that carries the XNU kernel. This is a structural check only; it
cannot prove that S-Boot or the z3s UEFI will run the image.

usage: z3s-boot-verify.py --image out/z3s-boot/z3s-boot.img \
           [--shim BootShim.bin] [--loader loader.efi] [--kernel mach]
"""
import argparse
import gzip
import hashlib
import struct
import sys
from pathlib import Path

FD_SIZE = 0x200000
FD_PAYLOAD_AT = 0x100000
FD_PAYLOAD_MAX = FD_SIZE - FD_PAYLOAD_AT
PAYLOAD_MAX = 0x00F00000
FD_BASE = 0xE8000000
PAYLOAD_BASE = 0xF0000000


def fail(message):
    print(f"z3s-boot-verify: {message}", file=sys.stderr)
    raise SystemExit(1)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def parse_xnup(data, where):
    if len(data) < 8:
        fail(f"{where}: shorter than an XNUP header")
    magic, size = struct.unpack_from("<II", data, 0)
    if magic != 0x50554E58:
        fail(f"{where}: magic {magic:#x}, expected XNUP")
    if size > len(data) - 8:
        fail(f"{where}: payload size {size:#x} exceeds {len(data) - 8:#x} bytes")
    entries = {}
    off = 8
    end = 8 + size
    while off + 24 <= end:
        name, entry_size, _reserved = struct.unpack_from("<16sII", data, off)
        name = name.split(b"\0", 1)[0].decode("ascii", "replace")
        if not name:
            break
        body = off + 24
        if body + entry_size > end:
            fail(f"{where}: entry {name!r} runs past the payload")
        entries[name] = data[body:body + entry_size]
        off = body + ((entry_size + 15) & ~15)
    if not entries:
        fail(f"{where}: no entries")
    return entries


def find_fd(blob, shim_size):
    if shim_size is not None:
        if blob[shim_size + 0x28:shim_size + 0x2c] == b"_FVH":
            return shim_size
        fail("the shim size does not point at an FD (_FVH missing)")
    # The shim is small; scan only the first 64 KiB for the FD signature.
    for off in range(0, min(len(blob), 0x10000) - 0x2c):
        if blob[off + 0x28:off + 0x2c] == b"_FVH":
            return off
    fail("no _FVH signature in the decompressed boot blob")


def check_shim(shim):
    if len(shim) < 0x40:
        fail("the shim is shorter than its 0x40-byte header")
    fd_base, fd_size = struct.unpack_from("<QQ", shim, 0x08)
    if fd_base != FD_BASE:
        fail(f"shim FD_BASE {fd_base:#x}, expected {FD_BASE:#x}")
    if fd_size != FD_SIZE:
        fail(f"shim FD_SIZE {fd_size:#x}, expected {FD_SIZE:#x}")
    # BootShim.S materialises the payload base with
    #   movz xN, #(PAYLOAD_BASE >> 16), lsl #16
    # because PAYLOAD_BASE is page-aligned below 4 GiB.
    want = 0xD2800000 | (1 << 21) | (((PAYLOAD_BASE >> 16) & 0xffff) << 5)
    for off in range(0, len(shim) - 4, 4):
        word = struct.unpack_from("<I", shim, off)[0]
        # Ignore Rd (bits 4-0): BootShim.S does not pin the register.
        if (word & 0xFFFFFFE0) == want:
            return
    fail(f"shim has no movz for PAYLOAD_BASE {PAYLOAD_BASE:#x}")


def check_loader(loader):
    """Reject a loader built for the FD-tail payload base.

    loader.c materialises PAYLOAD_BASE and PANIC_LOG_BASE as MOVZ immediates.
    A default build has PAYLOAD_BASE=0xE8100000 and PANIC_LOG_BASE=0xE8180000;
    the z3s build must have 0xF0000000 and 0xF0F00000. The panic-log immediate
    is the reliable discriminator: it only appears with the override.
    """
    found = set()
    for off in range(0, len(loader) - 4, 4):
        word = struct.unpack_from("<I", loader, off)[0]
        if (word & 0x7F800000) not in (0x52800000, 0x72800000):
            continue
        imm = (word >> 5) & 0xFFFF
        hw = (word >> 21) & 3
        found.add((imm, hw))
    if (PAYLOAD_BASE >> 16 & 0xFFFF, 1) not in found:
        fail(f"loader.efi does not materialise PAYLOAD_BASE {PAYLOAD_BASE:#x}")
    panic_base = (PAYLOAD_BASE + PAYLOAD_MAX) >> 16 & 0xFFFF
    if (panic_base, 1) not in found:
        fail(f"loader.efi does not materialise PANIC_LOG_BASE {PAYLOAD_BASE + PAYLOAD_MAX:#x}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--shim", help="BootShim.bin used to build the image")
    ap.add_argument("--loader", help="loader.efi expected in the FD tail")
    ap.add_argument("--kernel", help="Mach-O kernel expected after the FD")
    args = ap.parse_args()

    image = Path(args.image).read_bytes()
    if len(image) < 2048 or image[:8] != b"ANDROID!":
        fail("not an Android boot image")
    kernel_size = struct.unpack_from("<I", image, 8)[0]
    ramdisk_size = struct.unpack_from("<I", image, 16)[0]
    page_size = struct.unpack_from("<I", image, 36)[0]
    header_version = struct.unpack_from("<I", image, 40)[0]
    if header_version != 2:
        fail(f"header version {header_version}, expected 2")
    if page_size != 2048:
        fail(f"page size {page_size}, expected 2048")
    if kernel_size == 0:
        fail("empty kernel blob")
    kernel = image[page_size:page_size + kernel_size]
    if len(kernel) != kernel_size:
        fail("truncated kernel blob")
    try:
        blob = gzip.decompress(kernel)
    except OSError as exc:
        fail(f"kernel blob is not gzip: {exc}")

    shim = None
    if args.shim:
        shim = Path(args.shim).read_bytes()
        if blob[:len(shim)] != shim:
            fail("the boot blob does not start with the supplied shim")
        check_shim(shim)
    fd_off = find_fd(blob, len(shim) if shim is not None else None)
    if fd_off + FD_SIZE > len(blob):
        fail("the FD runs past the boot blob")
    fd = blob[fd_off:fd_off + FD_SIZE]
    fv_length = struct.unpack_from("<Q", fd, 0x20)[0]
    if fv_length != FD_SIZE:
        fail(f"FvLength {fv_length:#x}, expected {FD_SIZE:#x}")

    tail = fd[FD_PAYLOAD_AT:FD_PAYLOAD_AT + FD_PAYLOAD_MAX]
    tail_entries = parse_xnup(tail, "FD-tail payload")
    if "loader.efi" not in tail_entries:
        fail("FD-tail payload has no loader.efi")
    loader = tail_entries["loader.efi"]
    if loader[:2] != b"MZ":
        fail("FD-tail loader.efi is not a PE image")
    check_loader(loader)
    if args.loader:
        expected = Path(args.loader).read_bytes()
        if loader != expected:
            fail(f"FD-tail loader.efi SHA-256 {sha256(loader)} != {sha256(expected)}")

    after = fd_off + FD_SIZE
    if after >= len(blob):
        fail("no appended payload after the FD")
    appended = parse_xnup(blob[after:], "appended payload")
    if "kernel" not in appended:
        fail("appended payload has no kernel")
    kernel_image = appended["kernel"]
    if kernel_image[:4] != b"\xcf\xfa\xed\xfe":
        fail("appended kernel is not a 64-bit Mach-O")
    if len(kernel_image) > PAYLOAD_MAX:
        fail(f"kernel is {len(kernel_image)} bytes, larger than {PAYLOAD_MAX:#x}")
    if args.kernel:
        expected = Path(args.kernel).read_bytes()
        if kernel_image != expected:
            fail(f"appended kernel SHA-256 {sha256(kernel_image)} != {sha256(expected)}")
    if "cmdline" not in appended:
        fail("appended payload has no cmdline")
    cmdline = appended["cmdline"]
    if not cmdline.endswith(b"\0") or any(b < 0x20 or b > 0x7e for b in cmdline[:-1]):
        fail(f"cmdline is not printable ASCII: {cmdline!r}")

    print(f"image: {len(image)} bytes, header v{header_version}, page {page_size}")
    print(f"ramdisk: {ramdisk_size} bytes")
    print(f"blob: {len(blob)} bytes, FD at +{fd_off:#x}, FvLength {fv_length:#x}")
    print(f"shim: {len(shim) if shim else 'not supplied'} bytes, "
          f"FD {FD_BASE:#x}+{FD_SIZE:#x}, payload {PAYLOAD_BASE:#x}")
    print(f"FD-tail payload: {len(loader)} bytes loader.efi, sha256 {sha256(loader)}")
    print(f"appended payload: {len(kernel_image)} bytes kernel, sha256 {sha256(kernel_image)}")
    print(f"cmdline: {cmdline[:-1].decode('ascii')!r}")
    print("ok: z3s boot image structure")


if __name__ == "__main__":
    main()
