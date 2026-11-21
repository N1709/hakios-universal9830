#!/usr/bin/env python3
"""Repack the saved Samsung kernel with the bounded camera reference initramfs."""
from pathlib import Path
import gzip
import hashlib
import shutil
import struct
import subprocess

repo = Path(__file__).resolve().parents[2]
research = repo.parents[1]
audio = research / "work/stock-audio-test"
target = research / "work/stock-camera-test"
root = target / "root"
for directory in ("bin", "vendor/firmware", "proc", "sys", "dev", "tmp"):
    (root / directory).mkdir(parents=True, exist_ok=True)
copies = {
    repo / "tools/reference-boot/camera-init": root / "init",
    audio / "microot/bin/busybox": root / "bin/busybox",
    audio / "microot/bin/reboot-download": root / "bin/reboot-download",
    target / "camera-ref": root / "bin/camera-ref",
    research / "device/firmware/vendor-firmware/is_lib.bin": root / "vendor/firmware/is_lib.bin",
    research / "device/firmware/vendor-firmware/setfile_3j1.bin": root / "vendor/firmware/setfile_3j1.bin",
}
for source, dest in copies.items():
    if not source.is_file():
        raise SystemExit(f"missing {source}")
    if source != dest:
        shutil.copy2(source, dest)
for file in (root / "init", root / "bin/busybox", root / "bin/reboot-download", root / "bin/camera-ref"):
    file.chmod(0o755)

files = sorted(str(path.relative_to(root)) for path in root.rglob("*"))
cpio = subprocess.run(["/usr/bin/cpio", "-o", "-H", "newc"],
                      input=("\n".join(files) + "\n").encode(), cwd=root,
                      capture_output=True, check=True).stdout
ramdisk = gzip.compress(cpio, mtime=0)
old = (audio / "boot-mic-ref.img").read_bytes()
page = struct.unpack_from("<I", old, 36)[0]
kernel_size = struct.unpack_from("<I", old, 8)[0]
kernel = old[page:page + kernel_size]
dtb = (audio / "unpack/dtb").read_bytes()
if kernel != (audio / "unpack/kernel").read_bytes():
    raise SystemExit("reference kernel mismatch")
align = lambda size: (size + page - 1) // page * page
header = bytearray(old[:page])
struct.pack_into("<I", header, 16, len(ramdisk))
digest = hashlib.sha1()
for blob in (kernel, ramdisk, b"", b"", dtb):
    digest.update(blob)
    digest.update(struct.pack("<I", len(blob)))
header[576:608] = digest.digest() + bytes(12)
image = bytes(header)
for blob in (kernel, ramdisk, dtb):
    image += blob + bytes(align(len(blob)) - len(blob))
limit = (research / "device/backup/20261105-203534/boot.img").stat().st_size
if len(image) > limit:
    raise SystemExit(f"image too large for BOOT: {len(image)} > {limit}")
if b"init\0" not in cpio or b"bin/camera-ref\0" not in cpio:
    raise SystemExit("initramfs missing entry")
(target / "boot-camera-ref.img").write_bytes(image)
print(f"camera reference BOOT: {len(image)} bytes, limit {limit}, sha256 {hashlib.sha256(image).hexdigest()}")
