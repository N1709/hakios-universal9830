#!/bin/bash
# build.sh: out/RedwoodPhone.app (the CallKit provider for redwood-ims). Install: install.sh (phone lock holder).
set -e
cd "$(dirname "$0")"
sdk=${SDK:-"$HOME/theos/sdks/iPhoneOS16.5.sdk"}
app=out/RedwoodPhone.app
rm -rf out && mkdir -p "$app"
xcrun clang -target arm64-apple-ios16.0 -isysroot "$sdk" -O2 -Wall -fobjc-arc -o "$app/RedwoodPhone" main.m \
	-framework Foundation -framework CallKit -framework AVFoundation -framework UIKit -framework Intents
cp Info.plist "$app/"
codesign --force --sign - --timestamp=none --identifier com.redwood.phone --entitlements ent.plist "$app/RedwoodPhone"
echo "built $app"
