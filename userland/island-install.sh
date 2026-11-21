#!/bin/bash
# Build RedwoodIsland.dylib (src/island_tweak.m) and put it on the phone's tweak folder, then respring.
# The phone lock's holder only: CLAUDE_SESSION=NAME userland/island-install.sh [conf line, e.g. "y=5 w=81"]
# No conf line: the built-in default (pill top 5 pt) and any existing /var/jb/etc/redwood-island.conf stays.
set -euo pipefail
echo "island-install.sh: withdrawn (a /var/jb tweak can crash-loop SpringBoard, docs/devices/x1slte/dynamic-island.md)"; exit 1
cd "$(dirname "$0")"
SDK=${SDK:-${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk}
mkdir -p obj
clang -target arm64-apple-ios14.0 -isysroot "$SDK" -Wl,-no_fixup_chains -O2 -Wall -Wextra -Werror -fobjc-arc -dynamiclib \
	-framework Foundation -framework CoreGraphics \
	-install_name /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodIsland.dylib \
	src/island_tweak.m -o obj/RedwoodIsland.dylib
codesign -f -s - obj/RedwoodIsland.dylib
../../../tools/phone-lock.sh status | grep -q "held by ${CLAUDE_SESSION:-?} " ||
	{ echo "island-install.sh: built obj/RedwoodIsland.dylib; not installed (CLAUDE_SESSION must hold the phone)"; exit 0; }
scp -q obj/RedwoodIsland.dylib RedwoodIsland.plist s20:/var/jb/Library/MobileSubstrate/DynamicLibraries/
[ $# -gt 0 ] && ssh s20 "echo '$*' > /var/jb/etc/redwood-island.conf"
ssh s20 'rm -f /var/mobile/Library/Logs/redwood-island.log; killall -9 SpringBoard; sleep 12; cat /var/mobile/Library/Logs/redwood-island.log'
