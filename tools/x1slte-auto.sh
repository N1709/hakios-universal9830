#!/bin/zsh
# x1slte-auto.sh [system.apfs]   hands-free system-image update while iOS runs on Wi-Fi (uroot)
# x1slte-auto.sh --recovery      the same for out/x1slte-recovery.img (kernel, loader, root.apfs: uflash)
# x1slte-auto.sh --all           both (userland/system.apfs, then the recovery image), one restart
# logs saved -> phone to Log access (rwlogd /menu?to=1) -> uroot -> cold restart -> wait for iOS on Wi-Fi again.
cd "$(dirname "$0")/.."
# several Claude sessions, one phone (2026-11-06): only the lock's holder deploys (tools/phone-lock.sh, CLAUDE_SESSION)
if [ -n "${CLAUDE_SESSION:-}" ]; then
	$HOME/ios-port-research/tools/phone-lock.sh acquire "$CLAUDE_SESSION" || exit 3
else
	st=$($HOME/ios-port-research/tools/phone-lock.sh status); case "$st" in *held*) echo "[auto] $st (set CLAUDE_SESSION=NAME to deploy as the holder)"; exit 3 ;; esac
fi
# the phone over the USB cable when its network is up there (tools/phone-ip), else Wi-Fi; asked again after the restart
IP=$(phone-ip 2>/dev/null || echo ${Y2S_WIFI_IP:-10.77.0.2}); F=${1:-userland/system.apfs}; T=$(date +%Y%m%d-%H%M%S); D=logs/runs/$T; mkdir -p $D
up() { [ "$(curl -s -m 4 -o /dev/null -w '%{http_code}' http://$IP:8080/)" = 200 ]; }
wake() { ping -c1 -t1 $IP >/dev/null 2>&1; }
echo "[auto] phone at $IP ($([ "$IP" = "${Y2S_USB_IP:-10.77.0.2}" ] && echo USB cable || echo Wi-Fi))"
for i in $(seq 1 20); do wake; up && break; sleep 3; IP=$(phone-ip 2>/dev/null || echo $IP); done; up || { echo "[auto] phone not reachable (USB or Wi-Fi)"; exit 1; }
curl -s -m 60 http://$IP:8080/m2m > $D/m2m.txt; curl -s -m 90 http://$IP:8080/log > $D/kernel.txt
echo "[auto] logs saved: $D ($(wc -l < $D/m2m.txt) m2m records, $(wc -c < $D/kernel.txt) bytes of kernel log)"
echo "[auto] asking the phone for Log access"; curl -s -m 10 "http://$IP:8080/menu?to=1"; echo
for i in $(seq 1 60); do fastboot devices 2>/dev/null | grep -q fastboot && break; sleep 2; done
fastboot devices | grep -q fastboot || { echo "[auto] the phone did not reach Log access"; exit 1; }
./x1slte.sh ulog > $D/lastlog.txt 2>&1 || true
# --all: system.apfs and the recovery image in one Log access visit, one restart (A, 2026-11-14)
if [ "$F" = --all ]; then ./x1slte.sh uroot userland/system.apfs || exit 1; ./x1slte.sh uflash || exit 1
elif [ "$F" = --recovery ]; then ./x1slte.sh uflash || exit 1; else ./x1slte.sh uroot "$F" || exit 1; fi
# UP_PARAM=out/up_param-apple.img (A, 2026-11-15): Samsung's splash pictures too, in the same Log access visit. Needs a
# loader that takes up_param (2026-11-15 and later); an older one refuses it and nothing is written.
if [ -n "${UP_PARAM:-}" ]; then
	"${FASTBOOT:-fastboot}" stage "$UP_PARAM" > /dev/null 2>&1 && echo "[auto] up_param: $("${FASTBOOT:-fastboot}" oem write up_param 0 2>&1 | tr '\n' ' ')"
fi
./x1slte.sh ureboot
echo "[auto] waiting for iOS (USB cable or Wi-Fi)"
for i in $(seq 1 90); do sleep 4; IP=$(phone-ip 2>/dev/null) && {
	# the user's rule: the USB cable first. Wi-Fi often answers a few seconds before USB networking is up after a
	# restart: wait up to 40 s more for the cable before settling for Wi-Fi
	U=${Y2S_USB_IP:-10.77.0.2}
	for j in $(seq 1 10); do [ "$IP" = "$U" ] && break; sleep 4; IP=$(phone-ip 2>/dev/null || echo $IP); done
	echo "[auto] iOS is back after $((i*4)) s, at $IP ($([ "$IP" = "$U" ] && echo USB cable || echo "Wi-Fi: USB networking did not come up"))"; exit 0; }; done
echo "[auto] iOS did not come back within 6 min"; exit 2
