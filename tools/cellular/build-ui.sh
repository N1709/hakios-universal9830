#!/bin/bash
# Build only our Settings tweak. No system image, firmware or shared build tree.
set -euo pipefail
cd "$(dirname "$0")/../.."
CELLULAR_SDK=${CELLULAR_SDK:-${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk}
CELLULAR_OUT=${CELLULAR_OUT:-$PWD/out/cellular}
mkdir -p "$CELLULAR_OUT"
"${CC:-clang}" -target arm64-apple-ios16.5 -isysroot "$CELLULAR_SDK" \
    -O2 -Wall -Wextra -Werror -fobjc-arc -fblocks -dynamiclib \
    -F"$CELLULAR_SDK/System/Library/PrivateFrameworks" \
    -framework Foundation -framework UIKit -framework Preferences \
    -install_name /var/jb/usr/lib/TweakInject/RedwoodCellular.dylib \
    userland/src/cellular_settings.m -o "$CELLULAR_OUT/RedwoodCellular.dylib"
codesign -f -s - "$CELLULAR_OUT/RedwoodCellular.dylib"
cp tools/cellular/RedwoodCellular.plist "$CELLULAR_OUT/"
printf 'Cellular Settings UI: %s\n' "$CELLULAR_OUT"
