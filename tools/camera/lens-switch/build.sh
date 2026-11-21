#!/bin/sh
# Build/sign only; no phone access.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
out=${1:?output directory required}
mkdir -p "$out"
clang -target arm64-apple-ios16.5 -isysroot "${CAMERA_SDK:-$HOME/theos/sdks/iPhoneOS16.5.sdk}" -O2 -fobjc-arc -fblocks -Wall -Wextra -Werror -dynamiclib -framework Foundation -framework UIKit -framework CoreGraphics "$repo/userland/src/camera_lens_switch.m" -o "$out/RedwoodCameraLenses.dylib"
"${CAMERA_LDID:-$(command -v ldid || echo $repo/../../tools/bin/ldid)}" -S "$out/RedwoodCameraLenses.dylib"
python3 - "$out" <<'PY'
import pathlib,plistlib,sys
(pathlib.Path(sys.argv[1])/'RedwoodCameraLenses.plist').write_bytes(plistlib.dumps({'Filter':{'Bundles':['com.apple.camera']}}))
PY
