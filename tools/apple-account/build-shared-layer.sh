#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
AA_LAYER_SDK=${AA_LAYER_SDK:-$HOME/theos/sdks/iPhoneOS16.5.sdk}
AA_LAYER_OUT=${AA_LAYER_OUT:-out/apple-account/private}
mkdir -p "$AA_LAYER_OUT"
AA_LAYER_HASH=$(cat tools/apple-account/build-shared-layer.sh tools/apple-account/anisette-lab-bridge.m tools/apple-account/icloud-public-session.m tools/apple-account/icloud-delegate-session.m tools/apple-account/media-error-probe.m tools/apple-account/media-response-probe.m tools/apple-account/media-error-boundary.m tools/apple-account/media-signing-bridge.m tools/apple-account/*.h | shasum -a 256 | cut -c1-16)
AA_LAYER_TARGET="$AA_LAYER_OUT/libredwood-account-layer-$AA_LAYER_HASH.dylib"
if [ ! -f "$AA_LAYER_TARGET" ]; then
 AA_LAYER_TEMP=$(mktemp "$AA_LAYER_OUT/.account-layer-build.XXXXXX")
 trap 'rm -f "$AA_LAYER_TEMP"' EXIT HUP INT TERM
 clang -target arm64-apple-ios16.5 -isysroot "$AA_LAYER_SDK" -dynamiclib -fobjc-arc -fblocks -DRW_ACCOUNT_LAYER_BUNDLE=1 -framework Foundation -Wl,-no_fixup_chains -Wall -Wextra tools/apple-account/anisette-lab-bridge.m tools/apple-account/icloud-public-session.m tools/apple-account/icloud-delegate-session.m tools/apple-account/media-error-probe.m tools/apple-account/media-response-probe.m tools/apple-account/media-error-boundary.m tools/apple-account/media-signing-bridge.m -o "$AA_LAYER_TEMP"
 ldid -S "$AA_LAYER_TEMP"
 mv "$AA_LAYER_TEMP" "$AA_LAYER_TARGET"
fi
printf '%s\n' "$AA_LAYER_TARGET"
