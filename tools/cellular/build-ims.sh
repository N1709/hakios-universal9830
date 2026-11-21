#!/bin/bash
# build-ims.sh [OUT]: redwood-ims for the phone, with the AP voice path (ims-media.h: opencore-amr AMR-NB, static)
# opencore-amr 0.1.6 (Apache-2.0) from sourceforge, configured for arm64-apple-ios16 into work/third_party/amr-build/out
set -e
cd "$(dirname "$0")/../.."
sdk=${SDK:-"$HOME/theos/sdks/iPhoneOS16.5.sdk"}
amr=${AMR:-"$HOME/ios-port-research/work/third_party/amr-build/out"}
out=${1:-out/redwood-ims}
mkdir -p "$(dirname "$out")"
xcrun clang -target arm64-apple-ios16.0 -isysroot "$sdk" -O2 -Wall -Wextra -I"$amr/include" -o "$out" \
	tools/cellular/redwood-ims.c "$amr/lib/libopencore-amrnb.a" -lc++ -framework CoreFoundation
codesign --force --sign - --timestamp=none "$out"
echo "built $out"
