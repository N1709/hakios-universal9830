#!/bin/bash
# Build only the Files compatibility library, without touching a shared kernel build.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-"$repo/out/files"}
SDK=${SDK:-"$HOME/theos/sdks/iPhoneOS16.5.sdk"}
mkdir -p "$out"
if [[ $(uname -s) == Darwin ]]; then
    cc=(xcrun clang)
else
    cc=("${CC:-clang}")
fi
"${cc[@]}" -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -Wall -Wextra \
    -dynamiclib -install_name /redwood/lib/libredwood-files.dylib -lobjc -lsandbox \
    "$repo/userland/src/inject_files.c" -o "$out/libredwood-files.dylib"
"${cc[@]}" -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -Wall -Wextra \
    -fobjc-arc -framework Foundation "$repo/tools/storage/storage-probe.m" \
    -o "$out/storage-probe"
"${cc[@]}" -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -Wall -Wextra \
    "$repo/userland/src/local_files_host.c" -o "$out/LocalFilesHost"
"${cc[@]}" -target arm64-apple-ios16.0 -isysroot "$SDK" -O2 -Wall -Wextra \
    -fobjc-arc -fblocks -framework Foundation "$repo/tools/storage/check-operations.m" \
    -o "$out/check-operations"
if command -v "${LDID:-ldid}" >/dev/null; then
    "${LDID:-ldid}" -S "$out/libredwood-files.dylib"
    "${LDID:-ldid}" -S "$out/storage-probe"
    "${LDID:-ldid}" -S "$out/LocalFilesHost"
elif [[ $(uname -s) == Darwin ]]; then
    codesign -s - "$out/libredwood-files.dylib"
    codesign -s - "$out/storage-probe"
    codesign -s - "$out/LocalFilesHost"
fi
printf '%s\n' "$out/libredwood-files.dylib"
