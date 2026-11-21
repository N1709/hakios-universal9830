#!/bin/zsh
# flash-kernel.sh [KERNEL]   fast kernel-only test cycle (A, 2026-11-14): only the recovery partition's 16 MiB kernel slot
# at 0x2000000 changes -- loader, boot line, root.apfs, system image and userdata stay. Backs the running slot up first
# (out/fingerprint/slot-backup-<time>.bin), writes the new one with the loader's read-back, reboots, waits for SSH and
# prints the keybag state. KERNEL defaults to out/mach.x1slte.  Roll back: flash-kernel.sh out/fingerprint/slot-backup-*.bin
set -e
cd "$(dirname "$0")/../.."
K=${1:-out/mach.x1slte}
O=out/fingerprint; mkdir -p $O
T=$(date +%H%M%S)
python3 - "$K" $O/slot-new.bin <<'PY'
import sys
d=open(sys.argv[1],'rb').read()
assert len(d) <= 16*1024*1024, 'kernel larger than its slot'
open(sys.argv[2],'wb').write(d + b'\0' * (16*1024*1024 - len(d)))
PY
$HOME/ios-port-research/tools/phone-lock.sh status
if ! fastboot devices 2>/dev/null | grep -q fastboot; then
	curl -s -m 10 'http://10.77.0.2:8080/menu?to=1' >/dev/null || true
	for i in $(seq 1 40); do fastboot devices 2>/dev/null | grep -q fastboot && break; sleep 2; done
fi
fastboot devices | grep -q fastboot || { echo "flash-kernel: phone not in Log access"; exit 1; }
fastboot oem read recovery 2000000 1000000 >/dev/null 2>&1
fastboot get_staged $O/slot-backup-$T.bin >/dev/null 2>&1
echo "flash-kernel: running slot saved: $O/slot-backup-$T.bin"
fastboot stage $O/slot-new.bin >/dev/null 2>&1
fastboot oem write recovery 2000000 2>&1 | grep -q 'written and read back' || { echo "flash-kernel: WRITE NOT VERIFIED"; exit 1; }
echo "flash-kernel: written and read back; rebooting"
fastboot reboot >/dev/null 2>&1
for i in $(seq 1 90); do ssh -o ConnectTimeout=2 -o BatchMode=yes s20 true 2>/dev/null && break; sleep 2; done
sleep 5
ssh s20 '/var/root/native-keybag-status; uptime'
