#!/bin/sh
# Build/sign only. No phone access or installation.
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
artifact_dir=${1:?output directory required}
mkdir -p "$artifact_dir"
sdk=${CAMERA_SDK:-$HOME/theos/sdks/iPhoneOS16.5.sdk}
ldid=${CAMERA_LDID:-$(command -v ldid || echo $repo_root/../../tools/bin/ldid)}
clang -target arm64-apple-ios16.5 -isysroot "$sdk" -O2 -fobjc-arc -fblocks -Wall -Wextra -Werror \
 -framework Foundation -framework UIKit -framework IOKit -dynamiclib \
 "$repo_root/userland/src/camera_brightness_rule.m" -o "$artifact_dir/RedwoodCameraBrightness.dylib"
"$ldid" -S "$artifact_dir/RedwoodCameraBrightness.dylib"
python3 - "$artifact_dir" <<'PY'
import pathlib,plistlib,sys
p=pathlib.Path(sys.argv[1]);(p/'RedwoodCameraBrightness.plist').write_bytes(plistlib.dumps({
 'Filter':{'Bundles':['com.apple.camera','com.apple.springboard']}}))
PY
