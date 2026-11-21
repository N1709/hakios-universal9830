#!/bin/bash
# s20-firmware.sh: rebuild the gitignored Samsung/Broadcom/Cirrus firmware folders (fw-wifi, fw-sound,
# fw-haptic, fw-sensors) from YOUR phone's stock /vendor/firmware. These files are Samsung's: never commit them.
#
#   tools/s20-firmware.sh DIR       DIR = a copy of stock /vendor/firmware
#                                   (e.g. `adb pull /vendor/firmware` from Android/TWRP with vendor mounted,
#                                    or device/firmware/vendor-firmware from an earlier dump)
#   tools/s20-firmware.sh --adb     pull /vendor/firmware over adb first (Android or TWRP, vendor mounted)
#
# Each output file is checked against the SHA-256 the port was tested with (G980FXXSNHYB7); a different
# firmware revision is reported, not silently used.
set -euo pipefail
cd "$(dirname "$0")/.."
SRC=${1:-}
if [ "$SRC" = --adb ]; then
	SRC=$(mktemp -d); trap 'rm -rf "$SRC"' EXIT
	adb pull /vendor/firmware/. "$SRC" >/dev/null
fi
[ -d "$SRC" ] || { sed -n 2,11p "$0"; exit 2; }
# destination <- stock name, tested sha256 (first 16 hex digits)
MAP="
fw-wifi/bcm4375-sta.bin            bcmdhd_sta.bin_b1
fw-wifi/bcm4375-clm.blob           bcmdhd_clm.blob
fw-wifi/bcm4375-nvram-murata.txt   nvram.txt_1rh_es43_b1
fw-wifi/bcm4375-nvram-semco.txt    nvram.txt_CS01_semco_b1
fw-wifi/bcm4375B1-murata.hcd       bcm4375B1_murata.hcd
fw-wifi/bcm4375B1-semco.hcd        bcm4375B1_semco.hcd
fw-sound/abox_tplg.bin             abox_tplg.bin
fw-sound/calliope_dram.bin         calliope_dram.bin
fw-sound/calliope_dram_2.bin       calliope_dram_2.bin
fw-sound/calliope_sram.bin         calliope_sram.bin
fw-sound/calliope_sram_2.bin       calliope_sram_2.bin
fw-sound/vts.bin                   vts.bin
fw-haptic/cs40l25a.wmfw            cs40l25a.wmfw
fw-haptic/cs40l20.bin              cs40l20.bin
"
missing=0
while read -r dst src; do
	[ -n "$dst" ] || continue
	if [ ! -f "$SRC/$src" ]; then echo "missing in $SRC: $src (for $dst)"; missing=1; continue; fi
	mkdir -p "$(dirname "$dst")"
	install -m 0644 "$SRC/$src" "$dst"
	echo "$dst <- $src"
done <<< "$MAP"
# Sensor hub: built from Samsung's kernel-source firmware header (see tools/sensors/mk-fw-sensors.py).
if [ -f tools/sensors/mk-fw-sensors.py ]; then
	python3 tools/sensors/mk-fw-sensors.py --out fw-sensors || echo "fw-sensors: see tools/sensors/mk-fw-sensors.py --help"
else
	echo "fw-sensors: run tools/sensors/mk-fw-sensors.py (x1slte branch) to build bcm_sensorhub_a1.bin"
fi
exit $missing
