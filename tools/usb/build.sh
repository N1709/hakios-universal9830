#!/bin/bash
# Diagnostic only: does not enable a launch job or alter USB descriptors.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-"$repo/out/finder"}
sdk=${SDK:-"$HOME/theos/sdks/iPhoneOS16.5.sdk"}
mkdir -p "$out"
xcrun clang -target arm64-apple-ios16.0 -isysroot "$sdk" \
    -O2 -Wall -Wextra -Werror "$repo/tools/usb/lockdown-probe.c" \
    -o "$out/lockdown-probe"
"${LDID:-ldid}" -S "$out/lockdown-probe"
printf '%s\n' "$out/lockdown-probe"
