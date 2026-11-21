#!/bin/sh
# Build the native Camera dial and foreground brightness rule for image seeding.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
output=${1:?output directory required}
mkdir -p "$output"
sdk=${CAMERA_SDK:-$HOME/theos/sdks/iPhoneOS16.5.sdk}
ldid=${CAMERA_LDID:-$(command -v ldid || echo $repo/../../tools/bin/ldid)}
for spec in RedwoodCameraLenses:camera_lens_switch RedwoodCameraBrightness:camera_brightness_rule; do
 name=${spec%%:*}; source=${spec#*:}
 clang -target arm64-apple-ios16.5 -isysroot "$sdk" -O2 -fobjc-arc -fblocks \
  -dynamiclib -framework UIKit -framework Foundation -framework IOKit "$repo/userland/src/$source.m" -o "$output/$name.dylib"
 "$ldid" -S "$output/$name.dylib"
done
