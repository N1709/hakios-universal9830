#!/bin/sh
# Build a dual backend: original front by default, experimental front opt-in;
# rear always independent experimental backend. Build only: no phone changes.
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
artifact_dir=${1:?output directory required}
mkdir -p "$artifact_dir"
artifact_dir=$(CDPATH= cd -- "$artifact_dir" && pwd)
camera_sdk=${CAMERA_SDK:-$HOME/theos/sdks/iPhoneOS16.5.sdk}
camera_h264=${CAMERA_OPENH264:-$repo_root/../third-party/openh264}
camera_ldid=${CAMERA_LDID:-$(command -v ldid || echo $repo_root/../../tools/bin/ldid)}
python3 "$repo_root/tools/camera/prepare-profile-sources.py" "$artifact_dir"
camera_name=/var/redwood/libredwood-media-dev.dylib
camera_define=-DREDWOOD_DEV
if [ "${CAMERA_PERMANENT:-0}" = 1 ]; then
  camera_name=/redwood/lib/libredwood-media.dylib
  camera_define=-DREDWOOD_MEDIA_BUILTIN
fi
cd "$repo_root/userland"
clang -arch arm64 -target arm64-apple-ios16.5 -isysroot "$camera_sdk" -O2 \
  -I"$artifact_dir/original" -I src -c "$artifact_dir/original/camera_device.c" -o "$artifact_dir/original-device.o"
clang -arch arm64 -target arm64-apple-ios16.5 -isysroot "$camera_sdk" -O2 \
  -I"$artifact_dir/original" -I src -c "$artifact_dir/original/camera_isp.c" -o "$artifact_dir/original-isp.o"
clang -arch arm64 -target arm64-apple-ios16.5 -isysroot "$camera_sdk" -O2 \
  -I src -include "$artifact_dir/experimental-symbols.h" -c "$artifact_dir/experimental-device.c" -o "$artifact_dir/experimental-device.o"
clang -arch arm64 -target arm64-apple-ios16.5 -isysroot "$camera_sdk" -O2 \
  -I src -include "$artifact_dir/experimental-symbols.h" -c src/camera_isp.c -o "$artifact_dir/experimental-isp.o"
clang -arch arm64 -target arm64-apple-ios16.5 -isysroot "$camera_sdk" -O2 "$camera_define" \
  -I"$camera_h264/codec/api" -dynamiclib -install_name "$camera_name" \
  -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework IOSurface \
  -framework VideoToolbox -lobjc -lc++ -o "$artifact_dir/libredwood-media-dev.dylib" \
  src/inject_media.c src/h264_encoder.c "$artifact_dir/original-device.o" "$artifact_dir/original-isp.o" \
  "$artifact_dir/experimental-device.o" "$artifact_dir/experimental-isp.o" "$camera_h264/libopenh264.a"
"$camera_ldid" -S "$artifact_dir/libredwood-media-dev.dylib"
shasum -a 256 "$artifact_dir/libredwood-media-dev.dylib"
