#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
set +u
source common/macos-env.sh
set -u
sdk="${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk"
output="$PWD/out/fingerprint/ui"
mkdir -p "$output"
compiler="$PWD/xnu-deps/host-macos/bin/xnu-clang"
flags=(-arch arm64 -isysroot "$sdk" -miphoneos-version-min=16.0 -O2 -fobjc-arc -fblocks -Wall -Wextra -Werror)
"$compiler" "${flags[@]}" -framework Foundation -o "$output/redwood-fingerprint-ui" tools/fingerprint/ui/broker.m
"$compiler" "${flags[@]}" -dynamiclib -Wl,-install_name,/var/jb/redwood/lib/libredwood-fingerprint-ui.dylib \
  -framework Foundation -framework UIKit -framework QuartzCore -framework CoreGraphics -F"$sdk/System/Library/PrivateFrameworks" \
  -framework Preferences -o "$output/libredwood-fingerprint-ui.dylib" tools/fingerprint/ui/preview.m
/usr/bin/codesign -f -s - "$output/redwood-fingerprint-ui"
/usr/bin/codesign -f -s - "$output/libredwood-fingerprint-ui.dylib"
echo "Built rejection-only UI in $output; nothing installed."
