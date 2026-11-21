#!/bin/bash
# fetch-bkdm1.sh: Touch ID's sensor module for the system image, ported (A, 2026-11-20).
#
# The iPhone 8's /usr/lib/libBKDM1.dylib (Touch ID; the iPhone X image has only libBKDM2, Face ID) is cut out of the
# iPhone 8's iOS 16.5 (20F66) dyld shared cache and relinked to run on THIS image's iPhone X cache, so no 3 GB iPhone 8
# cache is needed on the phone:
#   1. the iPhone 8 cache from Apple's CDN (ipsw extract --dyld --remote; ~3 GB, deleted again unless KEEP=1)
#   2. DyldExtractor (dyldex) cuts libBKDM1 out of it
#   3. tools/touchid/relink_bkdm1.py relinks it (binds by name, pooled GOT and stubs repointed, classic method lists...),
#      with tools/touchid/bkdm1-map.json: the names of the cache's pooled pointer slots (20F66 iPhone10,1 cache)
#   4. ldid signs it
# Output: sysimage/iphone8/libBKDM1.dylib (~130 MB sparse, ~0.4 MB compressed); userland/build.sh puts it in /usr/lib.
# Apple's code: never in the repository (sysimage/ is ignored). Details: docs/devices/x1slte/touchid-port.md.
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=sysimage/iphone8/libBKDM1.dylib
[ -f "$OUT" ] && [ "${FORCE:-0}" != 1 ] && { echo "fetch-bkdm1: $OUT already there (FORCE=1 to redo)"; exit 0; }
IPSW=${IPSW8_URL:-https://updates.cdn-apple.com/2023SpringFCS/fullrestores/032-85662/3AF4C04F-D804-4EC7-8289-7DBD52EB3B9E/iPhone_4.7_P3_16.5_20F66_Restore.ipsw}
command -v ipsw >/dev/null || { echo "fetch-bkdm1: needs ipsw (brew install blacktop/tap/ipsw)"; exit 1; }
LDID=${LDID:-$(command -v ldid || echo ../../tools/bin/ldid)}

# Python with DyldExtractor, lief and capstone, in its own environment (once)
VENV=sysimage/iphone8/venv
if [ ! -x $VENV/bin/dyldex ]; then
	python3 -m venv $VENV
	$VENV/bin/pip -q install dyldextractor==2.2.2 lief==1.0.0 capstone==4.0.2
fi

work=sysimage/iphone8/dsc
mkdir -p "$work"
cache=$(find "$work" -name dyld_shared_cache_arm64 -type f | head -1)
if [ -z "$cache" ]; then
	echo "fetch-bkdm1: the iPhone 8's shared cache from Apple (about 3 GB, once)"
	ipsw extract --dyld --remote "$IPSW" --output "$work"
	cache=$(find "$work" -name dyld_shared_cache_arm64 -type f | head -1)
fi
[ -n "$cache" ] || { echo "fetch-bkdm1: no cache"; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
$VENV/bin/dyldex -e /usr/lib/libBKDM1.dylib -o "$tmp/libBKDM1.extracted.dylib" "$cache" >/dev/null
[ -s "$tmp/libBKDM1.extracted.dylib" ] || { echo "fetch-bkdm1: DyldExtractor failed"; exit 1; }
$VENV/bin/python3 -I tools/touchid/relink_bkdm1.py "$tmp/libBKDM1.extracted.dylib" "$tmp/libBKDM1.dylib" \
	tools/touchid/bkdm1-map.json | grep "^relink:"
"$LDID" -S "$tmp/libBKDM1.dylib"
mkdir -p "$(dirname "$OUT")"
mv "$tmp/libBKDM1.dylib" "$OUT"
[ "${KEEP:-0}" = 1 ] || rm -rf "$work"
echo "fetch-bkdm1: $OUT ($(wc -c < "$OUT") bytes, sparse)"
