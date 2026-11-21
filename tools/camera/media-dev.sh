#!/bin/sh
# Build libredwood-media as the development copy and load it into mediaserverd without a reboot
# (inject_media.c: /var/redwood/libredwood-media-dev.dylib is loaded instead of /redwood/lib's when it is there).
# usage: tools/camera/media-dev.sh [off]     (off: back to the flashed copy)
set -e
cd "$(dirname "$0")/../../userland"
if [ "$1" = off ]; then ssh s20 'rm -f /var/redwood/libredwood-media-dev.dylib; killall mediaserverd'; echo off; exit 0; fi
OH=${OPENH264_SRC:-../third-party/openh264}
../xnu-deps/host-macos/bin/xnu-clang -arch arm64 -isysroot $HOME/theos/sdks/iPhoneOS16.5.sdk -O2 -DREDWOOD_DEV -I$OH/codec/api \
  -dynamiclib -install_name /var/redwood/libredwood-media-dev.dylib -framework CoreFoundation -framework CoreMedia \
  -framework CoreVideo -framework IOSurface -framework VideoToolbox -lobjc -lc++ -o obj/libredwood-media-dev.dylib \
  src/inject_media.c src/camera_device.c src/camera_isp.c src/h264_encoder.c $OH/libopenh264.a
$HOME/ios-port-research/tools/bin/ldid -S obj/libredwood-media-dev.dylib
ssh s20 'cat > /var/redwood/libredwood-media-dev.dylib.new && mv /var/redwood/libredwood-media-dev.dylib.new /var/redwood/libredwood-media-dev.dylib && (killall Camera 2>/dev/null; sleep 2; killall mediaserverd)' < obj/libredwood-media-dev.dylib
echo "loaded (mediaserverd restarted)"
