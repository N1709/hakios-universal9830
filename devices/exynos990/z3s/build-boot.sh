#!/bin/bash
# Pack the z3s UEFI FD, loader.efi and the prebuilt XNU kernel into an Android
# boot image. Run this on the laptop or in CI, never on the phone.
#
#   ./devices/exynos990/z3s/build-boot.sh /path/to/SILICIUM_UEFI.fd [outdir]
#
# The image has two XNUP payloads:
#   FD + 0x100000  loader.efi only, for the UEFI XnuLoader hook (1 MiB slot)
#   0xF0000000     kernel + cmdline, copied there by the boot shim (15 MiB slot)
#
# Build the loader for the second window first:
#   PAYLOAD_BASE=0xF0000000 PAYLOAD_MAX_SIZE=0x00F00000 PANIC_LOG_BASE=0xF0F00000 \
#       ./xnuloader/build.sh
#
# The boot shim and mkbootimg come from the Mu-Silicium tree when MU is set;
# otherwise pass SHIM=, MKBOOTIMG= and RAMDISK= explicitly. The output is a raw
# image plus an Odin AP tar with recovery.img and its .md5, because the z3s
# chain (like x1slte) starts from the recovery partition.
set -euo pipefail
cd "$(dirname "$0")/../../.."

FD=${1:?usage: $0 <SILICIUM_UEFI.fd> [outdir]}
OUT=${2:-out/z3s-boot}
LOADER=${LOADER:-xnuloader/loader.efi}
KERNEL=${KERNEL:-prebuilt/mach.development.bcm2837}
DTB=${DTB:-devices/exynos990/z3s/uefi-z3s/Resources/DTBs/z3s.dtb}
CMDLINE_FILE=${CMDLINE_FILE:-devices/exynos990/z3s/z3s.cmdline}
IMAGE=z3s-recovery.img

die() { echo "build-boot.sh: $*" >&2; exit 1; }
[ -f "$FD" ] || die "no FD at $FD"
[ -f "$LOADER" ] || die "no loader at $LOADER (build xnuloader/build.sh first)"
[ -f "$KERNEL" ] || die "no kernel at $KERNEL"
[ -f "$DTB" ] || die "no DTB at $DTB"
[ -f "$CMDLINE_FILE" ] || die "no cmdline at $CMDLINE_FILE"
python3 - "$FD" <<'PY'
import sys
from pathlib import Path
fd = Path(sys.argv[1]).read_bytes()
if len(fd) != 0x200000:
    raise SystemExit(f"FD is {len(fd):#x} bytes, expected 0x200000")
if fd[0x28:0x2c] != b"_FVH":
    raise SystemExit("FD has no _FVH at 0x28")
if fd[0x100000:0x100004] != b"\xff\xff\xff\xff":
    print("build-boot.sh: warning: the FD tail is not erased; it will be replaced")
PY

mkdir -p "$OUT"
CMDLINE=$(tr '\n' ' ' < "$CMDLINE_FILE" | sed 's/[[:space:]]*$//')
[ -n "$CMDLINE" ] || die "the cmdline file is empty"

python3 xnuloader/mkpayload.py -o "$OUT/fd-payload.bin" loader.efi="$LOADER"
python3 xnuloader/mkpayload.py -o "$OUT/kernel-payload.bin" kernel="$KERNEL" --cmdline="$CMDLINE"

# The shim and mkbootimg come from the Mu-Silicium tree when it is present;
# otherwise pass SHIM=, MKBOOTIMG= and RAMDISK= explicitly.
if [ -n "${MU:-}" ]; then
	export SHIM=${SHIM:-$MU/BootShim/BootShim.bin}
	export MKBOOTIMG=${MKBOOTIMG:-$MU/Resources/Scripts/mkbootimg.py}
	export RAMDISK=${RAMDISK:-$MU/Resources/ramdisk}
fi
[ -n "${SHIM:-}" ] || die "set MU=... or SHIM=/path/to/BootShim.bin"
[ -n "${MKBOOTIMG:-}" ] || die "set MU=... or MKBOOTIMG=/path/to/mkbootimg.py"
[ -n "${RAMDISK:-}" ] || die "set MU=... or RAMDISK=/path/to/ramdisk"
[ -f "$SHIM" ] || die "no boot shim at $SHIM"
[ -f "$MKBOOTIMG" ] || die "no mkbootimg at $MKBOOTIMG"
[ -f "$RAMDISK" ] || die "no ramdisk at $RAMDISK"

UEFI_FD="$FD" DTB="$DTB" FD_BASE=0xE8000000 FD_SIZE=0x200000 \
	PAYLOAD_BASE=0xF0000000 FD_PAYLOAD_AT=0x100000 HEADER_VERSION=2 \
	python3 xnu/mkxnuimg.py --fd-payload "$OUT/fd-payload.bin" \
		-p "$OUT/kernel-payload.bin" -o "$OUT/$IMAGE"

python3 tools/z3s-boot-verify.py --image "$OUT/$IMAGE" \
	--shim "$SHIM" --loader "$LOADER" --kernel "$KERNEL"

# Odin AP tar: recovery.img is the partition the x1slte/z3s chain starts from.
# Samsung's .tar.md5 is the tar followed by the MD5 hex digest and a newline.
cp "$OUT/$IMAGE" "$OUT/recovery.img"
tar -C "$OUT" --mtime=@0 --owner=0 --group=0 --numeric-owner \
	-cf "$OUT/z3s-recovery.tar" recovery.img
cp "$OUT/z3s-recovery.tar" "$OUT/z3s-recovery.tar.md5"
md5sum "$OUT/z3s-recovery.tar" | cut -d' ' -f1 | tr -d '\n' >> "$OUT/z3s-recovery.tar.md5"
echo >> "$OUT/z3s-recovery.tar.md5"
rm -f "$OUT/recovery.img"

SHIM_ABS=$(realpath "$SHIM")
LOADER_ABS=$(realpath "$LOADER")
KERNEL_ABS=$(realpath "$KERNEL")
(
	cd "$OUT"
	sha256sum "$IMAGE" z3s-recovery.tar z3s-recovery.tar.md5 \
		fd-payload.bin kernel-payload.bin > SHA256SUMS
	python3 - "$IMAGE" "$SHIM_ABS" "$LOADER_ABS" "$KERNEL_ABS" <<'PY'
import hashlib, json, sys
from pathlib import Path

def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()

image, shim, loader, kernel = sys.argv[1:]
print(json.dumps({
    "device": "z3s",
    "image": image,
    "image_bytes": Path(image).stat().st_size,
    "image_sha256": digest(image),
    "odin_tar": "z3s-recovery.tar",
    "odin_tar_md5": Path("z3s-recovery.tar.md5").read_bytes()[-33:-1].decode("ascii"),
    "shim_sha256": digest(shim),
    "loader_sha256": digest(loader),
    "kernel_sha256": digest(kernel),
    "fd_payload_at": "0x100000",
    "kernel_payload_base": "0xF0000000",
    "kernel_payload_max": "0x00F00000",
    "hardware_tested": False,
    "installer_validated": False,
}, indent=2) + "\n", end="")
PY
) > "$OUT/build.json"

echo "build-boot.sh: $OUT/$IMAGE"
echo "build-boot.sh: $OUT/z3s-recovery.tar.md5"
