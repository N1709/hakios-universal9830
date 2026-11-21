#!/bin/bash
# get-inputs.sh — everything the release build needs that cannot be in the repository, fetched or made on this
# computer and checked against known hashes (inputs/, then ./build.sh lock-inputs). Run from the checkout's root.
#   inputs/tools/avbtool.py, testkey_rsa4096.pem   AOSP external/avb at a pinned commit (Apache 2.0)
#   inputs/x1slte/Mu-x1slte.img                    our UEFI (Mu-Silicium + the loader hook): JESVI_UEFI=file or URL,
#                                                  default the GitHub release asset (tools/install/README: inputs)
#   inputs/x1slte/boot-dtb.img                     Samsung's DT table, cut from YOUR phone's boot partition (TWRP
#                                                  over USB) or from a stock boot.img you give: JESVI_BOOT_IMG=file
#   inputs/toolchain.json                          this computer's compiler and the inputs' hashes
# Nothing of Samsung's or Apple's is downloaded here; the phone's own files come from the phone.
set -euo pipefail
case "${1:-}" in -h|--help) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 0;; esac
[ -f build.sh ] && [ -d tools/release ] || { echo "run this from the Jesvi iOS checkout's root"; exit 1; }
B=$'\033[1m' G=$'\033[32m' R=$'\033[31m' N=$'\033[0m'; [ -t 1 ] || B= G= R= N=
ok() { printf '%s  ok%s  %s\n' "$G" "$N" "$*"; }
die() { printf '%s  x %s%s\n' "$R" "$*" "$N" >&2; exit 1; }
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
mkdir -p inputs/tools inputs/x1slte

AVB=761178607206f4cb2af79ed9eec52d8cbd814adb
want_avb=e5a664a38db623da00f080219bc0ee60a640a9dc4a872803616fae4938ac749b
want_key=6a224754880a57ab9cbd308267cd157d94cf05a1c8cb851aec4090e045d24121
want_uefi=a1fe7661ba3604643f9a75ce7efc8eccfac529c7d7340f71323254f0f8086c15
want_dtb=9596875233beaf42706c3427aded6b981f4844e4f05b75ddd5f390c70c51b276   # SM-G980F; other firmware: warned only
UEFI_URL=${JESVI_UEFI_URL:-https://github.com/jesvi-eulah/Jesvi-iOS/releases/download/uefi-x1slte-$(cut -c1-8 <<<"$want_uefi")/Mu-x1slte.img}

printf '%sJesvi iOS: build inputs%s\n' "$B" "$N"
gs() { curl -fsSL -m 120 "https://android.googlesource.com/platform/external/avb/+/$AVB/$1?format=TEXT" | base64 -d > "$2"; }
[ "$(sha inputs/tools/avbtool.py 2>/dev/null)" = $want_avb ] || gs avbtool.py inputs/tools/avbtool.py
[ "$(sha inputs/tools/avbtool.py)" = $want_avb ] || die "avbtool.py: unexpected content from AOSP"
[ "$(sha inputs/tools/testkey_rsa4096.pem 2>/dev/null)" = $want_key ] || gs test/data/testkey_rsa4096.pem inputs/tools/testkey_rsa4096.pem
[ "$(sha inputs/tools/testkey_rsa4096.pem)" = $want_key ] || die "testkey_rsa4096.pem: unexpected content from AOSP"
ok "AVB tool and its public test key (AOSP ${AVB:0:8})"

u=inputs/x1slte/Mu-x1slte.img
if [ "$(sha $u 2>/dev/null)" != $want_uefi ]; then
	src=${JESVI_UEFI:-$UEFI_URL}
	if [ -f "$src" ]; then cp "$src" $u; else curl -fsSL -m 300 -o $u "$src" || die "UEFI: could not download $src (set JESVI_UEFI=file)"; fi
fi
[ "$(sha $u)" = $want_uefi ] || die "UEFI: $u is not the reviewed build (sha256 $want_uefi)"
ok "UEFI (Mu-x1slte.img)"

d=inputs/x1slte/boot-dtb.img
if [ ! -s $d ]; then
	img=${JESVI_BOOT_IMG:-}
	if [ -z "$img" ]; then
		adb get-state 2>/dev/null | grep -q recovery || die "boot-dtb: put the phone in TWRP over USB, or set JESVI_BOOT_IMG=stock boot.img"
		img=$(mktemp -t jesvi-boot); trap 'rm -f "$img"' EXIT
		echo "reading the phone's boot partition (60 MB) for Samsung's device tree table..."
		adb exec-out 'dd if=/dev/block/by-name/boot bs=4194304 2>/dev/null' > "$img"
	fi
	python3 -I - "$img" "$d" <<'PY' || die "boot-dtb: no Samsung DT table (magic d7b7ab1e) in that boot image"
import sys
data = open(sys.argv[1], 'rb').read()
i = data.find(bytes.fromhex('d7b7ab1e'))
if i < 0: sys.exit(1)
n = int.from_bytes(data[i + 4:i + 8], 'big')
if not 4096 <= n <= 4 << 20 or i + n > len(data): sys.exit(1)
open(sys.argv[2], 'wb').write(data[i:i + n])
PY
fi
[ "$(sha $d)" = $want_dtb ] && ok "Samsung DT table (boot-dtb.img, the qualified firmware's)" ||
	printf '%s  !%s  boot-dtb.img is from a different firmware than the qualified one: the build goes on, untested\n' "$R" "$N"

cc=$(/usr/bin/xcrun clang --version 2>/dev/null | head -4 || clang --version | head -4)
python3 -I - "$cc" "$(sha inputs/tools/avbtool.py)" "$(sha inputs/tools/testkey_rsa4096.pem)" "$(sha $u)" "$(sha $d)" <<'PY'
import json, sys
cc, avb, key, uefi, dtb = sys.argv[1:]
json.dump({'schema': 1, 'purpose': 'local private build qualification; not a public redistributable input set',
           'compiler': cc, 'uefi': {'sha256': uefi, 'verification': 'tools/install/get-inputs.sh: reviewed hash'},
           'inputs': {'tools/avbtool.py': avb, 'tools/testkey_rsa4096.pem': key,
                      'x1slte/Mu-x1slte.img': uefi, 'x1slte/boot-dtb.img': dtb}},
          open('inputs/toolchain.json', 'w'), indent=2)
PY
ok "toolchain.json"
if [ -d sysimage/rootfs ]; then ./build.sh lock-inputs >/dev/null && ok "input lock (inputs/private-inputs.lock.json)"
else echo "  (the input lock is made once iOS 16.5's files are here: sysimage/fetch.sh, then run this again)"; fi
