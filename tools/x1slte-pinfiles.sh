#!/bin/bash
# x1slte-pinfiles.sh: the iOS root and /private/var as pinned files in Android's userdata (SM-G980F).
#
#   tools/x1slte-pinfiles.sh create [ROOT_MB] [VAR_MB]   /data/darwin/system.img and var.img (default 2600, 16384):
#                                                        pinned + preallocated, one extent each, recorded in
#                                                        common/devices/x1slte.conf
#   tools/x1slte-pinfiles.sh map                         print the extents again and check them against the conf
#   tools/x1slte-pinfiles.sh write-root FILE             stream FILE (userland/system.apfs) into system.img IN PLACE
#                                                        (dd conv=notrunc: an adb push would replace the file and lose
#                                                        the pin), then read it back and compare
#   tools/x1slte-pinfiles.sh remove                      delete /data/darwin (iOS's files only; Android untouched)
#
# Needs the phone in TWRP (adb as root) or rooted Android, and tools/phone/pinfile (ios-port-research).
# Writes only inside /data/darwin through the filesystem; never a raw partition.
set -euo pipefail
cd "$(dirname "$0")/.."
CONF=common/devices/x1slte.conf
PINFILE=${PINFILE:-$HOME/ios-port-research/tools/phone/pinfile}
die() { echo "x1slte-pinfiles.sh: $*" >&2; exit 1; }
sh_() { adb shell "$@" | tr -d '\r'; }

check_phone() {
	adb get-state >/dev/null 2>&1 || die "no adb device"
	[ "$(sh_ id -u)" = 0 ] || die "adb is not root (use TWRP)"
	sh_ 'mount' | grep -q ' /data type f2fs' || die "/data is not mounted as f2fs (TWRP: Mount -> Data)"
	sh_ 'grep -q "Samsung Z3S\|exynos990" /proc/device-tree/compatible /proc/device-tree/model 2>/dev/null' || true
	[ "$(sh_ getprop ro.boot.bootloader)" = G980FXXSNHYB7 ] || die "bootloader is not G980FXXSNHYB7: not the phone this conf was measured on"
	adb push "$PINFILE" /tmp/pinfile >/dev/null && sh_ chmod 755 /tmp/pinfile
}

# extent PATH -> "PHYSICAL LENGTH" (bytes), or dies unless exactly one extent
extent() {
	local out
	out=$(sh_ "/tmp/pinfile map $1") || die "$1: not one contiguous extent:\n$out"
	echo "$out" | awk '/^extent 0/ { for (i = 1; i <= NF; i++) { split($i, kv, "="); if (kv[1] == "physical") p = kv[2]; if (kv[1] == "length") l = kv[2] } } END { print p, l }'
}

set_conf() { # set_conf KEY VALUE
	python3 - "$CONF" "$1" "$2" <<'PY'
import re, sys
p, k, v = sys.argv[1:4]
s = open(p).read()
s, n = re.subn(r'^%s=[^\n#]*' % re.escape(k), '%s=%s ' % (k, v), s, flags=re.M)
assert n == 1, k
open(p, 'w').write(s)
PY
}

cmd_create() {
	local root_mb=${1:-2600} var_mb=${2:-16384}
	check_phone
	sh_ '[ ! -e /data/darwin ]' || die "/data/darwin exists already: 'map' it, or 'remove' it first"
	local free; free=$(sh_ "df -k /data | tail -1" | awk '{print $4}')
	[ $(( (root_mb + var_mb + 4096) * 1024 )) -lt "$free" ] || die "not enough free space in /data"
	sh_ 'mkdir -p /data/darwin && chmod 700 /data/darwin'
	sh_ "/tmp/pinfile create /data/darwin/system.img $root_mb" || die "system.img: not pinned or not contiguous"
	sh_ "/tmp/pinfile create /data/darwin/var.img $var_mb" || die "var.img: not pinned or not contiguous"
	record
}

record() {
	local rp rl vp vl
	read -r rp rl < <(extent /data/darwin/system.img)
	read -r vp vl < <(extent /data/darwin/var.img)
	(( rp % 4096 == 0 && vp % 4096 == 0 )) || die "extents not 4 KiB aligned"
	set_conf ROOT_UD_OFF "$((rp))"
	set_conf ROOT_UD_LEN "$((rl))"
	set_conf VAR_UD_FIRST "$((vp / 4096))"
	set_conf VAR_UD_BLOCKS "$((vl / 4096))"
	echo "system.img: userdata+$(printf '0x%x' $((rp))) ($((rl >> 20)) MiB)"
	echo "var.img:    userdata blocks $((vp / 4096))..$(((vp + vl) / 4096)) ($((vl >> 20)) MiB)"
	echo "recorded in $CONF"
}

cmd_map() {
	check_phone
	# shellcheck disable=SC1090
	. "$CONF"
	local rp rl vp vl
	read -r rp rl < <(extent /data/darwin/system.img)
	read -r vp vl < <(extent /data/darwin/var.img)
	[ "$((rp))" = "$ROOT_UD_OFF" ] && [ "$((vp / 4096))" = "$VAR_UD_FIRST" ] || die "the files moved, or the conf is stale: run create again"
	echo "extents match $CONF"
}

cmd_write_root() {
	local f=${1:?usage: write-root FILE}
	check_phone
	# shellcheck disable=SC1090
	. "$CONF"
	local size; size=$(wc -c < "$f")
	[ "$size" -le "$ROOT_UD_LEN" ] || die "$f ($size B) is larger than system.img ($ROOT_UD_LEN B)"
	echo "write-root: $f ($((size >> 20)) MiB) into /data/darwin/system.img in place"
	# Not a pipe into dd: TWRP's toybox dd, reading a pipe, wrote the short last block's tail but left the 2 MiB
	# before it as zeros (seen 2026-11-05). So the file goes over whole first, then dd copies file to file.
	local tmp=/data/darwin/.incoming
	adb push "$f" "$tmp" >/dev/null || die "push failed"
	sh_ "dd if=$tmp of=/data/darwin/system.img bs=1048576 conv=notrunc,fsync 2>&1 | tail -1"
	sh_ "rm -f $tmp"
	local here there
	here=$(shasum -a 256 < "$f" | cut -d' ' -f1)
	there=$(sh_ "head -c $size /data/darwin/system.img | sha256sum" | cut -d' ' -f1)
	[ "$here" = "$there" ] || die "read back differs: phone $there, file $here"
	cmd_map >/dev/null
	echo "write-root: verified ($here); extent unchanged"
}

cmd_remove() {
	check_phone
	sh_ 'rm -rf /data/darwin' && echo "removed /data/darwin"
	for k in ROOT_UD_OFF ROOT_UD_LEN VAR_UD_FIRST VAR_UD_BLOCKS; do set_conf "$k" ""; done
}

case "${1:-}" in
create) shift; cmd_create "$@" ;;
map) cmd_map ;;
write-root) shift; cmd_write_root "$@" ;;
remove) cmd_remove ;;
*) sed -n '2,13p' "$0"; exit 1 ;;
esac
