#!/bin/sh
# Run inside the isolated ARM64 Linux VM, with only its private /share mounted.
# Start the phone relay and host TCP proxy first. This is not an iOS boot job.
set -eu
umask 077
duration=${1:-115}
assist=${2:-}
[ "$#" -le 2 ] && { [ -z "$assist" ] || [ "$assist" = --assist-time ]; } || exit 2
case "$duration" in ''|*[!0-9]*) echo 'duration must be 10..600 seconds' >&2; exit 2;; esac
[ "$duration" -ge 10 ] && [ "$duration" -le 600 ] || exit 2
[ "$(uname -s)" = Linux ] && [ -x /share/android-runner ] || {
    echo 'run this in the GNSS Linux lab VM' >&2; exit 2;
}
run=/share/session-$(date +%s)
mkdir "$run"
mkdir -p /share/android/dev /share/android/proc /dev/socket
mountpoint -q /share/android/dev || mount --bind /dev /share/android/dev
mountpoint -q /share/android/proc || mount -t proc proc /share/android/proc
# Never replace a previous live session or a stale regular file with a command.
if pidof linker64 lhd-lab-bridge >/dev/null; then
    echo 'another lab engine/bridge is running' >&2; exit 1
fi
control=/share/android/data/system/gps/CtrlPipe
if [ -e "$control" ] && [ ! -p "$control" ]; then
    echo 'CtrlPipe is not a FIFO; inspect the stale lab state first' >&2; exit 1
fi
engine= bridge= reader= logger=
cleanup() {
    trap - EXIT HUP INT TERM
    # Closing the bridge makes the phone relay perform its paired navigation stop.
    for child in "$bridge" "$reader" "$engine" "$logger"; do
        [ -z "$child" ] || kill "$child" 2>/dev/null || true
    done
    wait 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM
if ! pidof log-capture >/dev/null; then
    /share/log-capture > "$run/logger.log" 2>&1 & logger=$!
fi
for endpoint in to_server to_client; do
    fifo=/share/android/data/vendor/gps/.pipe.gpsd_to_lhd.$endpoint
    [ -e "$fifo" ] || mkfifo "$fifo"
    [ -p "$fifo" ] || { echo 'LHD endpoint is not a FIFO' >&2; exit 1; }
done
# Open the command reader before gpsd emits its power/start commands. Starting
# gpsd first can silently lose those commands and leave only pass-through data.
/share/lhd-lab-bridge "$duration" > "$run/bridge.log" 2>&1 & bridge=$!
ready=0
while ! grep -q '^FIFO server ready$' "$run/bridge.log"; do
    kill -0 "$bridge" 2>/dev/null || { echo 'bridge exited before ready' >&2; exit 1; }
    [ "$ready" -lt 50 ] || { echo 'bridge readiness timed out' >&2; exit 1; }
    sleep 0.1
    ready=$((ready + 1))
done
/share/android-runner > "$run/gpsd.log" 2>&1 & engine=$!
ready=0
while [ "$ready" -lt 50 ]; do
    kill -0 "$engine" 2>/dev/null || { echo 'engine exited before ready' >&2; exit 1; }
    [ ! -p "$control" ] || break
    sleep 0.1
    ready=$((ready + 1))
done
[ -p "$control" ] || { echo 'engine did not create CtrlPipe' >&2; exit 1; }
nmea=/share/android/data/vendor/gps/nmea
[ -e "$nmea" ] || mkfifo "$nmea"
[ -p "$nmea" ] || { echo 'NMEA endpoint is not a FIFO' >&2; exit 1; }
cat "$nmea" > "$run/receiver.nmea" & reader=$!
if [ "$assist" = --assist-time ]; then
    # Stock IrmSetRefTime accepts week/tow (milliseconds), not "utc".
    # GPS-UTC=18s through 2026: USNO leap-second page and IERS Bulletin C72.
    # Expire the lab assumption rather than silently reusing it in later years.
    [ "$(date +%Y)" = 2026 ] || { echo 'recheck GPS-UTC offset before time assistance' >&2; exit 1; }
    gps_seconds=$(( $(date +%s) - 315964800 + 18 ))
    week=$((gps_seconds / 604800))
    tow=$((gps_seconds % 604800 * 1000))
    timeout 5 sh -c 'printf '\''$pglirm,ref_time,week,%s,tow,%s,unc_ms,5000\n'\'' "$2" "$3" > "$1"' sh "$control" "$week" "$tow"
fi
# Bound opening the FIFO too: a failed engine must not leave the shell hanging.
timeout 5 sh -c 'printf '\''$pglirm,req_pos,redwood,period,1000\n'\'' > "$1"' sh "$control"
echo "lab capture: $run"
wait "$bridge"
bridge=
