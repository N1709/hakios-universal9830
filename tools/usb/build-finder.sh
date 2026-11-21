#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-"$repo/out/finder-local"}
sdk=${SDK:-"$HOME/theos/sdks/iPhoneOS16.5.sdk"}
mkdir -p "$out"
xcrun clang -target arm64-apple-ios16.0 -isysroot "$sdk" \
    -F "$sdk/System/Library/PrivateFrameworks" -O2 -Wall -Wextra -Werror \
    -dynamiclib -install_name /redwood/lib/libredwood-finder.dylib \
    -framework CoreFoundation -framework MobileActivation \
    "$repo/userland/src/inject_finder.c" -o "$out/libredwood-finder.dylib"
"${LDID:-ldid}" -S "$out/libredwood-finder.dylib"
printf '%s\n' "$out/libredwood-finder.dylib"
