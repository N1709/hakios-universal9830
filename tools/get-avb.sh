#!/bin/bash
# Fetch the AOSP AVB tool and its public test key, checked against the
# pinned commit's hashes. build-boot.sh needs both to add the hash footer
# that Samsung's sboot expects on recovery/boot (x1slte: without it the
# phone drops to Download Mode even with vbmeta verification off).
#
#   tools/get-avb.sh [inputs/tools]
#
# Nothing here is secret: both files are Apache-2.0 AOSP test data.
set -euo pipefail
cd "$(dirname "$0")/.."

DEST=${1:-inputs/tools}
AVB=761178607206f4cb2af79ed9eec52d8cbd814adb
WANT_TOOL=e5a664a38db623da00f080219bc0ee60a640a9dc4a872803616fae4938ac749b
WANT_KEY=6a224754880a57ab9cbd308267cd157d94cf05a1c8cb851aec4090e045d24121

die() { echo "get-avb.sh: $*" >&2; exit 1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }
fetch() { # url dest
	curl -fsSL -m 120 "$1?format=TEXT" | base64 -d > "$2"
}

mkdir -p "$DEST"
if [ "$(sha "$DEST/avbtool.py" 2>/dev/null)" != "$WANT_TOOL" ]; then
	fetch "https://android.googlesource.com/platform/external/avb/+/$AVB/avbtool.py" "$DEST/avbtool.py"
fi
[ "$(sha "$DEST/avbtool.py")" = "$WANT_TOOL" ] || die "avbtool.py: unexpected content from AOSP"

if [ "$(sha "$DEST/testkey_rsa4096.pem" 2>/dev/null)" != "$WANT_KEY" ]; then
	fetch "https://android.googlesource.com/platform/external/avb/+/$AVB/test/data/testkey_rsa4096.pem" "$DEST/testkey_rsa4096.pem"
fi
[ "$(sha "$DEST/testkey_rsa4096.pem")" = "$WANT_KEY" ] || die "testkey_rsa4096.pem: unexpected content from AOSP"

echo "get-avb.sh: avbtool.py + testkey_rsa4096.pem (AOSP ${AVB:0:8}) in $DEST"
