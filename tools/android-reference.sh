#!/bin/bash
# android-reference.sh [OUT] — while the S20 runs an Android ROM (USB debugging on), record what we compare Jesvi iOS
# against: CPU/GPU/memory frequency governors and limits, cpuidle, scheduler, thermal zones and trips, the power
# HAL's init files, and the fingerprint stack's log during a live enrol + unlocks. No root needed. The output holds
# device details: it goes to ~/ios-port-research/device/dump/ (never the repository).
#   android-reference.sh            snapshot + fingerprint log (press Enter when the finger tests are done)
#   FP_SECONDS=300 android-reference.sh   the same, the fingerprint log for 300 s
set -uo pipefail
OUT=${1:-$HOME/ios-port-research/device/dump/android-reference-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"
a() { adb shell "$@" 2>/dev/null | tr -d '\r'; }
adb get-state 2>/dev/null | grep -q device || { echo "no Android over USB: turn on USB debugging and accept the prompt"; exit 1; }
echo "recording into $OUT"
{
	echo "# props"; a getprop | grep -E 'ro.build.(display|fingerprint)|ro.product.model|ro.boot.(bootloader|em.model)|vendor.power|thermal|sched|perf'
	for p in /sys/devices/system/cpu/cpufreq/policy*; do
		echo "# $p"; a "cd $p && for f in affected_cpus scaling_governor scaling_min_freq scaling_max_freq cpuinfo_min_freq cpuinfo_max_freq scaling_available_frequencies scaling_cur_freq; do echo \"\$f: \$(cat \$f)\"; done"
		a "for f in $p/*/; do for g in \$f*; do [ -f \$g ] && echo \"\$g: \$(cat \$g 2>/dev/null)\"; done; done"
	done
	echo "# cpuidle"; a 'for s in /sys/devices/system/cpu/cpu0/cpuidle/state* /sys/devices/system/cpu/cpu4/cpuidle/state* /sys/devices/system/cpu/cpu6/cpuidle/state*; do echo "$s: $(cat $s/name) latency $(cat $s/latency) residency $(cat $s/residency) disable $(cat $s/disable)"; done'
	echo "# cpusets"; a 'for c in /dev/cpuset/*/cpus; do echo "$c: $(cat $c)"; done'
	echo "# uclamp / schedtune"; a 'for f in /dev/cpuctl/*/cpu.uclamp.* /dev/stune/*/schedtune.*; do echo "$f: $(cat $f 2>/dev/null)"; done'
	echo "# sched sysctl"; a 'for f in /proc/sys/kernel/sched_*; do echo "$f: $(cat $f 2>/dev/null)"; done'
	echo "# devfreq (memory, bus, GPU)"; a 'for d in /sys/class/devfreq/*; do echo "== $d"; for f in governor min_freq max_freq cur_freq available_frequencies available_governors; do echo "$f: $(cat $d/$f 2>/dev/null)"; done; done'
	echo "# GPU (Mali)"; a 'for f in /sys/kernel/gpu/* /sys/devices/platform/*mali*/*; do [ -f $f ] && echo "$f: $(cat $f 2>/dev/null | head -c 300)"; done'
	echo "# thermal zones"; a 'for z in /sys/class/thermal/thermal_zone*; do echo "== $z $(cat $z/type) temp $(cat $z/temp) policy $(cat $z/policy 2>/dev/null)"; for t in $z/trip_point_*_temp; do echo "  $t: $(cat $t) $(cat ${t%_temp}_type 2>/dev/null) hyst $(cat ${t%_temp}_hyst 2>/dev/null)"; done; done'
	echo "# cooling devices"; a 'for c in /sys/class/thermal/cooling_device*; do echo "$c: $(cat $c/type) cur $(cat $c/cur_state)/$(cat $c/max_state)"; done'
	echo "# battery"; a 'for f in temp capacity current_now voltage_now status; do echo "$f: $(cat /sys/class/power_supply/battery/$f)"; done'
} > "$OUT/system.txt"
echo "  system.txt: $(wc -l < "$OUT/system.txt") lines"
mkdir -p "$OUT/etc"
for f in $(a 'ls /vendor/etc/init/*.rc /vendor/etc/*power*.xml /vendor/etc/*thermal* /vendor/etc/init/hw/*.rc /system/etc/init/*power*.rc 2>/dev/null'); do
	adb pull "$f" "$OUT/etc/" >/dev/null 2>&1
done
grep -l -i -E "cpufreq|governor|thermal|sched|devfreq|uclamp|cpuset|perf|power" "$OUT"/etc/* 2>/dev/null | sed 's#.*/#  etc/#' | head -30
echo
echo "fingerprint: recording the log now. On the phone: register one finger, lock, unlock with it 3-4 times, once"
echo "with a wrong finger. Press Enter here when done (FP_SECONDS=N: record N seconds instead)."
adb logcat -c
adb logcat -v threadtime > "$OUT/logcat-all.txt" 2>&1 &
L=$!
if [ -n "${FP_SECONDS:-}" ]; then sleep "$FP_SECONDS"; else read -r _; fi
kill $L 2>/dev/null; sleep 1
grep -i -E "finger|bauth|biometric|fpc|qbt|tee|tzdaemon|teegris|sensor_hal|FpSensor|ifaa|samsung.hardware.biometrics" "$OUT/logcat-all.txt" > "$OUT/logcat-fingerprint.txt"
echo "  logcat-fingerprint.txt: $(wc -l < "$OUT/logcat-fingerprint.txt") lines (all: $(wc -l < "$OUT/logcat-all.txt"))"
echo "done: $OUT"
