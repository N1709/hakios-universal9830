#!/bin/bash
# build.sh: out/redwood-telephonyd, the phone's telephony call provider (CommCenter's CallKit identity). install.sh puts it on the phone.
set -e
cd "$(dirname "$0")"
sdk=${SDK:-"$HOME/theos/sdks/iPhoneOS16.5.sdk"}
mkdir -p out
xcrun clang -target arm64-apple-ios16.0 -isysroot "$sdk" -O2 -Wall -Wextra -Wno-unused-parameter -fobjc-arc -o out/redwood-telephonyd telephonyd.m \
	-framework Foundation -framework CallKit -framework AVFoundation
codesign --force --sign - --timestamp=none --identifier com.apple.coretelephony --entitlements ent.plist out/redwood-telephonyd
echo "built out/redwood-telephonyd"
