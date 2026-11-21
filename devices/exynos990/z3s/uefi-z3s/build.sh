#!/bin/bash
# Build the z3s UEFI firmware volume inside a Mu-Silicium tree.
#
#   ./build.sh [/path/to/Mu-Silicium]
#   MU=/path/to/Mu-Silicium ./build.sh
#
# The tree must have its submodules checked out (Common/Mu, Mu_Basecore,
# Silicium-ACPI, Binaries, OpenSSL) and the EDK2 Python packages installed.
# The result is Build/z3sPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd.
set -euo pipefail
cd "$(dirname "$0")"

here=$(pwd)
repo=$(cd ../../../.. && pwd)
mu=${MU:-${1:-$repo/Mu-Silicium}}

die() { echo "build.sh: $*" >&2; exit 1; }

[ -f "$mu/build_uefi.py" ] || die "no Mu-Silicium tree at $mu (set MU=...)"
[ -d "$mu/Platforms/Samsung" ] || die "$mu has no Platforms/Samsung"
[ -d "$mu/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib" ] ||
	die "$mu has no Silicium BootManagerLib"
[ -d "$mu/BootShim" ] || die "$mu has no BootShim"
[ -d "$mu/Resources" ] || die "$mu has no Resources"

python3 -c 'import edk2toolext' 2>/dev/null ||
	die "EDK2 Python packages are missing (pip install -r $mu/pip-requirements.txt)"

echo "build.sh: overlay -> $mu"
cp -a Platforms/Samsung/z3sPkg "$mu/Platforms/Samsung/"
cp -a BootManagerLib/. "$mu/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/"
cp -a BootShim.S "$mu/BootShim/BootShim.S"
cp -a Resources/. "$mu/Resources/"

# EDK2's CLANGPDB toolchain takes the target prefix from the environment.
export CLANGPDB_AARCH64_PREFIX=${CLANGPDB_AARCH64_PREFIX:-aarch64-linux-gnu-}
export GCC_AARCH64_PREFIX=${GCC_AARCH64_PREFIX:-aarch64-linux-gnu-}

echo "build.sh: build_uefi.py -d z3s -r RELEASE"
cd "$mu"
python3 build_uefi.py -d z3s -r RELEASE

fd="$mu/Build/z3sPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd"
[ -f "$fd" ] || die "the build produced no $fd"
echo "build.sh: $fd"
sha256sum "$fd"
