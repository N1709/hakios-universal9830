#!/bin/sh
# island-preset.sh [NAME]   pick a Dynamic Island pill preset on the phone (respring only, no reboot).
# Without NAME: list the presets and show the phone's current setting. See docs/devices/x1slte/dynamic-island.md.
set -e
cd "$(dirname "$0")/island-presets"
if [ -z "$1" ]; then
	for f in *.conf; do printf '  %-16s %s\n' "${f%.conf}" "$(cat "$f")"; done
	printf 'phone now:         %s\n' "$(ssh s20 cat /var/jb/etc/redwood-island.conf 2>/dev/null || echo '(no file: built-in default)')"
	exit 0
fi
[ -f "$1.conf" ] || { echo "no preset $1 (run without arguments for the list)"; exit 1; }
ssh s20 "cat > /var/jb/etc/redwood-island.conf && (/var/jb/usr/bin/sbreload >/dev/null 2>&1 || killall -9 SpringBoard)" < "$1.conf"
echo "island preset $1: $(cat "$1.conf") (SpringBoard restarting)"
