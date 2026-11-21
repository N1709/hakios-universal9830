#!/bin/sh
# Build/sign the development camera library locally. No install, SSH or service restart.
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
artifact_dir=${1:-$repo_root/out/camera-quality}
mkdir -p "$artifact_dir"
artifact_dir=$(CDPATH= cd -- "$artifact_dir" && pwd)
camera_sdk=${CAMERA_SDK:-$HOME/theos/sdks/iPhoneOS16.5.sdk}
camera_h264=${CAMERA_OPENH264:-$repo_root/../third-party/openh264}
camera_ldid=${CAMERA_LDID:-$(command -v ldid || echo $repo_root/../../tools/bin/ldid)}
camera_cc=${CC:-clang}
[ -d "$camera_sdk" ] && [ -f "$camera_h264/libopenh264.a" ] || {
  echo 'Set CAMERA_SDK and CAMERA_OPENH264 to the local iOS SDK and built OpenH264 tree.' >&2
  exit 1
}
cd "$repo_root/userland"
"$camera_cc" -arch arm64 -target arm64-apple-ios16.5 -isysroot "$camera_sdk" -O2 -DREDWOOD_DEV \
  -I"$camera_h264/codec/api" -dynamiclib -install_name /var/redwood/libredwood-media-dev.dylib \
  -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework IOSurface \
  -framework VideoToolbox -lobjc -lc++ -o "$artifact_dir/libredwood-media-dev.dylib" \
  src/inject_media.c src/camera_device.c src/camera_isp.c src/h264_encoder.c "$camera_h264/libopenh264.a"
if [ -x "$camera_ldid" ]; then
  "$camera_ldid" -S "$artifact_dir/libredwood-media-dev.dylib"
fi
shasum -a 256 "$artifact_dir/libredwood-media-dev.dylib"
