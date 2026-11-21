#!/bin/bash
# build-calls.sh [install]: libredwood-calls.dylib (src/calls_tweak.c + src/phone_dial.h); "install" copies it to the phone
# and restarts the Phone app and the call screen (no reboot). Never while a call is up.
set -e
cd "$(dirname "$0")"
source ../common/macos-env.sh 2>/dev/null || true
xnu-clang -arch arm64 -isysroot ${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk -O1 -dynamiclib \
	-install_name /var/jb/redwood/lib/libredwood-calls.dylib -framework CoreFoundation -lobjc \
	-o obj/libredwood-calls.dylib src/calls_tweak.c 2>&1 | grep -E 'error' || true
codesign -f -s - obj/libredwood-calls.dylib 2>/dev/null
echo "built obj/libredwood-calls.dylib"
if [ "${1:-}" = install ]; then
	ssh s20 '! grep -q "^active" /var/tmp/redwood-cell/call 2>/dev/null' || { echo "a call is up: not installed"; exit 1; }
	scp -q -O obj/libredwood-calls.dylib s20:/var/jb/redwood/lib/libredwood-calls.dylib.new
	ssh s20 'mkdir -p /var/jb/redwood/lib; mv -f /var/jb/redwood/lib/libredwood-calls.dylib.new /var/jb/redwood/lib/libredwood-calls.dylib; sync;
	  killall -9 InCallService 2>/dev/null; echo "installed; call screen restarted (the Phone app picks it up at its next start)"'
fi
