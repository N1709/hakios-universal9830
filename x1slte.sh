#!/bin/bash
# x1slte.sh: build, pack, flash, and read back the kernel log for the Galaxy S20 (SM-G980F, x1slte).
# Derived from y2s.sh. Differences: a 20 MiB kernel slot (root.apfs at 52 MiB, up to 14.4 MB), the iOS system image
# from a pinned file in userdata (common/devices/x1slte.conf), never y2s's super offset.
#
#   ./x1slte.sh kernel                 build XNU (GIC_V2 exported), copy to out/mach.x1slte, export xnu/
#   ./x1slte.sh payload "<cmdline>"    rebuild the XNUP payload with this cmdline, splice it into out/x1slte-embed.img
#   ./x1slte.sh cmdline                print the cmdline currently in out/x1slte-embed.img
#   ./x1slte.sh pack                   out/x1slte-recovery.img = embed@0 + kernel@32 MiB + root.apfs@48 MiB
#   ./x1slte.sh flash                  adb: write out/x1slte-recovery.img to recovery, verify, reboot into it
#   ./x1slte.sh log                    adb: pull the 8 MiB log at recovery+16 MiB, print it (also ../xnulog.txt)
#   ./x1slte.sh mlog                   the last run's Metal and display lines only (USB if the phone is
#                                   in Log access, else Wi-Fi), so a log can be read for those alone
#
# mali2metal (PLANS/10-mali2metal.md), with no key press while iOS is up on Wi-Fi:
#   ./x1slte.sh loop [label]           payload (x1slte.cmdline) + pack; the phone hands itself to Log access
#                                   (rwlogd /menu); uflash; cold restart; wait for iOS; the standard tour;
#                                   the Metal channel saved to logs/m2m/ and reported against the last run.
#                                   M2M_VIA=download: through Download Mode and heimdall instead (for a
#                                   phone whose kernel predates the fixed kern.redwood_menu). M2M_PROF=1: with
#                                   backboardd's sampling profiler over the tour (tools/prof-symbolize.py RUN)
#   ./x1slte.sh m2m <fetch|report|tour|wait-tour|tail> ...   tools/m2m.py on its own
#   ./x1slte.sh ship "<cmdline>"       payload + pack + flash
#
# Without Android (PLANS/09-standalone.md): the loader's menu -> "Log access (USB)", then
#   ./x1slte.sh ulog                   the last run's log over USB (also ../xnulog.txt)
#   ./x1slte.sh uflash                 out/x1slte-recovery.img to recovery over USB, read back by the phone
#                                   (and the boot image to `boot` too once `standalone` was done)
#   ./x1slte.sh uboot                  leave Log access and boot iOS
#   ./x1slte.sh hflash                 from Samsung's Download Mode: heimdall writes recovery (and boot)
#
# While iOS runs, over Wi-Fi (rwlogd on the phone, port 8080, Bonjour "HackiOS"; or Y2S_IP=address):
#   ./x1slte.sh wlog                   the kernel's log so far (also ../xnulog.txt)
#   ./x1slte.sh wtail                  follow it live (Ctrl-C ends)
#   ./x1slte.sh hlogo                  (Download Mode) the boot splash: the Apple logo instead of Samsung's
#   ./x1slte.sh standalone             (once, asks first) the boot image into `boot`: power-on goes to the
#                                   menu, Android no longer boots (return to Android: full stock restore; see docs/release/rollback.md)
#
# Without a cmdline, payload and ship use DEFAULT_CMDLINE: the configuration measured best so far
# (PLANS/01-performance.md). Never give cpus= and cpumask= together: XNU panics.
#
# Partition layout: PLANS/00-foundations.md, handoff.md section 4.
set -euo pipefail
cd "$(dirname "$0")"

EMBED=out/x1slte-embed.img
PAYLOAD=out/x1slte-payload.bin
PAYLOAD_AT=$((0x100870))                # FD at page + 112 = 0x870, payload at FD + 0x100000
PAYLOAD_MAX=$((0x80000))
DEFAULT_CMDLINE=$(cat devices/exynos990/x1slte/x1slte.cmdline)   # tracked working profile, including software-passcode persistence
KERNEL_BUILD=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837

die() { echo "x1slte.sh: $*" >&2; exit 1; }

cmd_kernel() {
	set +u
	. common/macos-env.sh
	set -u
	export GIC_V2=1
	# several Claude sessions share this one build tree: one kernel build at a time (tools/team.sh, lock "build")
	if [ -n "${CLAUDECODE:-}" ]; then
		local team=$HOME/ios-port-research/tools/team.sh who
		who=$("$team" name)
		"$team" lock "$who" build "./x1slte.sh kernel" || die "another session is building the kernel (tools/team.sh who)"
		trap '"$HOME/ios-port-research/tools/team.sh" unlock "'"$who"'" build >/dev/null' EXIT
	fi
	touch out/.kernel-start
	local build_rc=0
	./xnu/xnu-build.sh -j"$(sysctl -n hw.ncpu)" -k build > out/kernel-build.log 2>&1 || build_rc=$?
	if [ "$build_rc" -ne 0 ]; then
		tail -n 35 out/kernel-build.log >&2
		die "kernel build failed (exit $build_rc); refusing stale kernel artifact"
	fi
	if grep -E '^[^ ]*\.(c|cpp|s|h):[0-9]+:[0-9]+: (fatal error|error|warning):' out/kernel-build.log; then
		die "compiler errors/warnings above (full log: out/kernel-build.log)"
	fi
	[ "$KERNEL_BUILD" -nt out/.kernel-start ] || echo "kernel: nothing changed, not relinked"
	strings "$KERNEL_BUILD" | grep 'gic: v2' > /dev/null || die "kernel is not GICv2 (GIC_V2 not exported?)"
	cp "$KERNEL_BUILD" out/mach.x1slte
	# The loader reads the kernel into a 16 MiB window (xnuloader DOWNLOAD_MAX_SIZE: 0xF0000000 up to the display at
	# 0xF1000000): a larger file loses its tail, which is __LINKEDIT (seen 2026-11-05: 16918224 bytes, 141 KB of symbols
	# lost). The local symbols go (strip -x): about 14.7 MB, the global symbols kept for backtraces.
	/usr/bin/xcrun strip -x out/mach.x1slte
	[ "$(wc -c < out/mach.x1slte)" -le $((0x1000000)) ] || die "kernel larger than the loader's 16 MiB read window"
	xnu/export.sh || echo "kernel: xnu/export.sh failed (needs GNU cp: brew install coreutils); xnu/ overlay not refreshed from xnu-src/"
	echo "kernel: out/mach.x1slte ($(wc -c < out/mach.x1slte) bytes)"
}

cmd_payload() {
	local cmdline=${1:-$DEFAULT_CMDLINE}
	case " $cmdline " in
	*" cpus="*" cpumask="* | *" cpumask="*" cpus="*) die "cpus= and cpumask= together make XNU panic" ;;
	esac
	# (A, 2026-11-20) a boot line copied into a scratch script long ago had ufs_win=0:15625000 instead of
	# ufs_userdata_all=1: iOS saw a window of the data volume. Take devices/exynos990/x1slte/x1slte.cmdline + extras.
	case " $cmdline " in
	*" ufs_win="*) [ "${WIN_OK:-0}" = 1 ] || die "ufs_win= in the boot line: the S20 uses ufs_userdata_all=1 (devices/exynos990/x1slte/x1slte.cmdline); WIN_OK=1 if on purpose" ;;
	esac
	local rsize
	# shellcheck disable=SC1091
	. common/devices/x1slte.conf
	# this phone's own identifiers (EFS): kept out of git in x1slte.local.conf
	[ -f common/devices/x1slte.local.conf ] && . common/devices/x1slte.local.conf
	[ "${ROOT_PART:-userdata}" = super ] || { [ -n "${ROOT_UD_OFF:-}" ] && [ -n "${ROOT_UD_LEN:-}" ]; } ||
		die "ROOT_UD_OFF/ROOT_UD_LEN are empty in common/devices/x1slte.conf: create /data/darwin/system.img with tools/phone/pinfile first"
	root_window
	ROOT_UD_OFF_HEX=$(printf '%x' "$ROOT_OFF"); ROOT_UD_LEN_HEX=$(printf '%x' "$ROOT_LEN")
	# the loader: super's real tail always; super+0..ROOT_SUPER_LEN when the conf has it (iOS only, 2026-11-06: the
	# system volume where Android's system/vendor were); userdata only in system.img's extent while that is in use
	# the boot logo (A, 2026-11-15): iOS's own, from the iOS root, into xnuloader/applelogo.h, the UEFI's logo slot and
	# out/up_param-apple.img; git-ignored outputs (Apple artwork). No iOS root: the loader's screen stays black.
	# DEVMODE=1 (A, 2026-11-15): the loader's JesviOS menu, countdown and text at every boot, for development. Set it in
	# common/devices/x1slte.local.conf (this Mac only, git-ignored) or the environment. Off: iOS's logo, no menu (end
	# users). Release builds (tools/release sets JESVI_TARGET) never get it.
	local devmode=${DEVMODE:-0}
	[ -n "${JESVI_TARGET:-}" ] && devmode=0
	[ "$devmode" = 1 ] && echo "payload: dev mode (the JesviOS menu at boot)"
	# (the UEFI's line under the logo reads "- JESVI -" in both modes; mkbootlogo --dev would keep Mu's "[Volume Up] Boot Manager")
	python3 -I tools/bootlogo/mkbootlogo.py || echo "payload: no boot logo (tools/bootlogo/mkbootlogo.py failed)"
	# iPhone's charging pictures for S-Boot's phone-off screens (Apple's art from the IPSW, fetched once)
	python3 -I tools/bootlogo/mkcharging.py || echo "payload: no charging pictures (tools/bootlogo/mkcharging.py failed)"
	# CELLULAR_RESERVE_CP=1 by default (2026-11-13: two flashes built without it left cellular with "IPC map refused":
	# no LTE, no VoLTE). CELLULAR_RESERVE_CP= (empty) builds the loader without the CP reservation.
	env GIC_V2=1 CELLULAR_RESERVE_CP="${CELLULAR_RESERVE_CP-1}" SUPER_TAIL_START="$SUPER_TAIL_START" ${ROOT_SUPER_LEN:+SUPER_ROOT_LEN="$(printf '0x%x' "$ROOT_SUPER_LEN")"} \
		${ROOT_UD_LEN:+USERDATA_WIN_OFF="$(printf '0x%x' "$ROOT_UD_OFF")" USERDATA_WIN_LEN="$(printf '0x%x' "$ROOT_UD_LEN")"} \
		xnuloader/build.sh > /dev/null
	# the placeholders of x1slte.cmdline: the root window (read only) and /private/var's write window
	case "$cmdline" in *@VAR_WIN@*)
		[ -n "${VAR_UD_FIRST:-}" ] && [ -n "${VAR_UD_BLOCKS:-}" ] ||
			die "VAR_UD_FIRST/VAR_UD_BLOCKS are empty in common/devices/x1slte.conf: create /data/darwin/var.img first" ;;
	esac
	[ -n "${BT_ADDR:-}" ] && case " $cmdline " in *" bt_addr="*) ;; *) cmdline="$cmdline bt_addr=$BT_ADDR" ;; esac
	# boot_args.CommandLine is 608 bytes (xnuloader BOOT_LINE_LENGTH, pexpert arm64/boot.h) and the loader puts
	# "debug=0x8 rd=md0 ufs_shared_pa=0x...  " (~52) in front: past ~550 here the END of the line is cut off silently
	# (2026-11-06: touch_lat and bt_addr were lost that way)
	cmdline=${cmdline//@ROOT_PART@/$ROOT_PART}
	cmdline=${cmdline//@ROOT_OFF@/$ROOT_UD_OFF_HEX}
	cmdline=${cmdline//@ROOT_LEN@/$ROOT_UD_LEN_HEX}
	cmdline=${cmdline//@VAR_WIN@/${VAR_UD_FIRST:-}:${VAR_UD_BLOCKS:-}}
	# Validate the resolved command, rather than the longer placeholder names.
	[ ${#cmdline} -le 550 ] || die "boot line is ${#cmdline} characters (most 550: the loader adds ~52 and XNU keeps 607): shorten x1slte.cmdline"
	# the loader for this phone: GIC_V2 (the GICv3 system registers are undefined here: a loader built
	# without it takes a UEFI exception reading ICC_PMR_EL1 on the way into the kernel)
	rsize=$(printf '%x' "$(wc -c < userland/root.apfs)")     # (root.apfs grows with GL=test: libOSMesa)
	python3 xnuloader/mkpayload.py -o "$PAYLOAD" loader.efi=xnuloader/loader.efi $([ "$devmode" = 1 ] && echo --devmode) \
		--kpart recovery --koff 2000000 --rpart recovery --roff 3400000 --rsize "$rsize" \
		--rdpart userdata --rdoff "$ROOT_UD_OFF_HEX" --rdsize "$ROOT_UD_LEN_HEX" ${cmdline:+--cmdline "$cmdline"}
	python3 - "$EMBED" "$PAYLOAD" "$PAYLOAD_AT" "$PAYLOAD_MAX" <<'PY'
import sys
embed, payload, at, limit = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
img = bytearray(open(embed, 'rb').read())
p = open(payload, 'rb').read()
assert img[at:at + 4] == b'XNUP' or img[at:at + limit] == b'\xff' * limit, 'no XNUP payload (and no erased slot) at the expected offset of %s' % embed
assert len(p) <= limit, 'payload too large'
img[at:at + limit] = b'\xff' * limit      # the FD's erased fill
img[at:at + len(p)] = p
open(embed, 'wb').write(img)
PY
	echo "payload: cmdline '$cmdline' spliced into $EMBED"
}

cmd_embed() {
	local src=${1:-${UEFI_IMAGE:-inputs/x1slte/Mu-x1slte.img}}
	[ -f "$src" ] || die "no $src: download the x1slte-uefi artifact of github.com/jesvi-eulah/g980f-uefi"
	mkdir -p out
	python3 - "$src" "$PAYLOAD_AT" "$PAYLOAD_MAX" <<'PY'
import struct, sys
img = open(sys.argv[1], 'rb').read(); at, limit = int(sys.argv[2]), int(sys.argv[3])
assert img[:8] == b'ANDROID!' and struct.unpack('<I', img[36:40])[0] == 2048, 'not a page-2048 Android boot image'
assert img[0x870 + 0x28:0x870 + 0x2c] == b'_FVH', 'no firmware volume at page + 112'
assert img[at:at + limit] in (b'\xff' * limit,) or img[at:at + 4] == b'XNUP', 'the payload slot (FD + 0x100000) is in use'
PY
	cp "$src" "$EMBED"
	echo "embed: $EMBED from $src"
}

cmd_cmdline() {
	python3 - "$EMBED" "$PAYLOAD_AT" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read(); i = int(sys.argv[2])
assert d[i:i + 4] == b'XNUP'
i += 8
while True:
    name = d[i:i + 16].rstrip(b'\0'); size = struct.unpack('<I', d[i + 16:i + 20])[0]
    if not name:
        break
    if name == b'cmdline':
        print(d[i + 24:i + 24 + size].rstrip(b'\0').decode())
    i += 24 + (size + 15) // 16 * 16
PY
}

cmd_pack() {
	# (A, 2026-11-15) a stale userland/root.apfs (Nov 14: the old com.redwood.phone job, no com.redwood.telephonyd) was
	# packed and flashed: calls sat at "Starting..." until the right root went back. Refuse a root without the phone's
	# call provider job; ROOT_OK=1 packs it anyway (a test root on purpose).
	# (A, 2026-11-20) a root from plain userland/build.sh (no GPU layer, minimal jobs) was flashed: black wallpaper,
	# stutter, iCloud and apps down. The phone's root comes from userland/build-gpu-y2s.sh with REST=1.
	if [ "${ROOT_OK:-0}" != 1 ] && ! { LC_ALL=C grep -q -a 'libOSMesa' userland/root.apfs && LC_ALL=C grep -q -a 'libredwood-gl.dylib' userland/root.apfs; }; then
		echo "x1slte.sh: pack: userland/root.apfs has no GPU layer (libOSMesa / libredwood-gl.dylib): built by plain userland/build.sh?"
		echo "  Rebuild it: REST=1 JESVI_TARGET=x1slte IOS_INPUT_DIR=../sysimage userland/build-gpu-y2s.sh; ROOT_OK=1 to pack it anyway."
		return 1
	fi
	if [ "${ROOT_OK:-0}" != 1 ] && ! LC_ALL=C grep -q -a 'com.redwood.telephonyd' userland/root.apfs; then
		echo "x1slte.sh: pack: userland/root.apfs has no com.redwood.telephonyd job (stale root: calls would stay at"
		echo "  \"Starting...\"). Rebuild it (REST=1 JESVI_TARGET=x1slte IOS_INPUT_DIR=../sysimage userland/build-gpu-y2s.sh) or take the phone's own root (docs/devices/x1slte/root-swap.md);"
		echo "  ROOT_OK=1 to pack it anyway."
		return 1
	fi
	python3 - "$EMBED" "$PAYLOAD_AT" <<'PY'
import struct, sys
ker = open('out/mach.x1slte', 'rb').read()
rd = open('userland/root.apfs', 'rb').read()
img = open('out/x1slte-embed.img', 'rb').read()
assert len(img) <= 0x1000000 and len(ker) <= 0x1400000, 'embed > 16 MiB or kernel > 20 MiB'
assert 0x3400000 + len(rd) <= 69009408, 'root.apfs does not fit in recovery (65.8 MiB)'
# The loader reads exactly the payload's rsize bytes of root.apfs: a rebuilt image of another size
# packed without `payload` boots to "apfs: container 0: N blocks of 4096 do not fit the device"
# and halts on the Silicium screen (25 October).
i = int(sys.argv[2]) + 8
rsize = None
while True:
    name = img[i:i + 16].rstrip(b'\0'); size = struct.unpack('<I', img[i + 16:i + 20])[0]
    if not name:
        break
    if name == b'rsize':
        rsize = int(img[i + 24:i + 24 + size].rstrip(b'\0'), 16)
    i += 24 + (size + 15) // 16 * 16
if rsize != len(rd):
    sys.exit('x1slte.sh: pack: the payload says root.apfs is %s bytes and it is %d: run '
             '`./x1slte.sh payload "<cmdline>"` again first' % (rsize, len(rd)))
out = bytearray(max(0x2000000 + len(ker), 0x3400000 + len(rd)))
out[0:len(img)] = img
out[0x2000000:0x2000000 + len(ker)] = ker
out[0x3400000:0x3400000 + len(rd)] = rd
open('out/x1slte-recovery.img', 'wb').write(out)
print('pack: out/x1slte-recovery.img (%d bytes)' % len(out))
PY
	# The DTB section as this phone's sboot reads it: a Samsung DT table (magic d7b7ab1e) like its stock boot.img and
	# TWRP carry, not a bare FDT -- "DT table header check fail ... FDT_ERR_BADMAGIC", then Download Mode (2026-11-05).
	# Swapped in at the image's end, the header's dtb_size and id (mkbootimg's SHA-1) made to match.
	# shellcheck disable=SC1091
	. common/devices/x1slte.conf
	[ -f "$DTBH_FILE" ] || die "no DTBH_FILE ($DTBH_FILE): extract it from the phone's boot.img"
	python3 - out/x1slte-recovery.img "$DTBH_FILE" <<'PY'
import hashlib, struct, sys
p, dtbh = sys.argv[1], open(sys.argv[2], 'rb').read()
d = bytearray(open(p, 'rb').read())
assert d[:8] == b'ANDROID!' and struct.unpack('<I', d[40:44])[0] == 2 and dtbh[:4] == bytes.fromhex('d7b7ab1e')
ps = struct.unpack('<I', d[36:40])[0]; pg = lambda n: (n + ps - 1) // ps * ps
ks, rs, ss = (struct.unpack('<I', d[o:o + 4])[0] for o in (8, 16, 24))
rec_sz = struct.unpack('<I', d[1632:1636])[0]; dsz = struct.unpack('<I', d[1648:1652])[0]
koff = ps; roff = koff + pg(ks); soff = roff + pg(rs); recoff = soff + pg(ss); doff = recoff + pg(rec_sz)
end = doff + pg(dsz)
assert end <= 0x2000000, 'boot image runs into the kernel slot'
d[doff:end] = bytes(end - doff)
d[doff:doff + len(dtbh)] = dtbh
struct.pack_into('<I', d, 1648, len(dtbh))
struct.pack_into('<Q', d, 1652, 0x10000000)   # dtb_addr as the stock boot.img and TWRP have it (Mu-Silicium's is 0x11f00000)
assert doff + pg(len(dtbh)) <= 0x1000000
h = hashlib.sha1()
for off, n in ((koff, ks), (roff, rs), (soff, ss), (recoff, rec_sz), (doff, len(dtbh))):
    h.update(bytes(d[off:off + n])); h.update(struct.pack('<I', n))
d[576:576 + 32] = h.digest().ljust(32, b'\0')
open(p, 'wb').write(d)
print('pack: DTB section is the Samsung DT table (%d bytes), header id updated' % len(dtbh))
PY
	# An AVB hash footer, as TWRP's recovery has: this phone's sboot drops to Download Mode without one (2026-11-05),
	# even with vbmeta's verification off (flags 3), and hangs at the Samsung logo when the hash does not match what is
	# on the flash. The loader writes its log into recovery (16 MiB) after every run, and uflash writes around it, so the
	# hash covers only the boot image (shim + UEFI + payload), which nothing changes at runtime; the kernel (32 MiB),
	# root.apfs (52 MiB) and the log lie outside the hashed range. Signed like TWRP's, with AOSP's public test key
	# (external/avb/test/data/testkey_rsa4096.pem): the key matters not, the footer does.
	local avb=${AVBTOOL:-inputs/tools/avbtool.py} key=${AVB_TEST_KEY:-inputs/tools/testkey_rsa4096.pem}
	[ -f "$avb" ] && [ -f "$key" ] || die "set AVBTOOL and AVB_TEST_KEY to the pinned AOSP tool and public test key"
	python3 - out/x1slte-recovery.img out/.avb-head.img <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read()
ps = struct.unpack('<I', d[36:40])[0]; pg = lambda n: (n + ps - 1) // ps * ps
ks, rs, ss = (struct.unpack('<I', d[o:o + 4])[0] for o in (8, 16, 24))
rec, dsz = struct.unpack('<I', d[1632:1636])[0], struct.unpack('<I', d[1648:1652])[0]
n = ps + pg(ks) + pg(rs) + pg(ss) + pg(rec) + pg(dsz)          # the boot image, as sboot reads it
n = (n + 4095) // 4096 * 4096
assert n < 0x1000000, n
open(sys.argv[2], 'wb').write(d[:n])
print('pack: AVB hash over the boot image only: %d bytes' % n)
PY
	python3 -I "$avb" add_hash_footer --image out/.avb-head.img --partition_size 69009408 \
		--partition_name recovery --algorithm SHA256_RSA4096 --key "$key" --rollback_index 1
	python3 - out/.avb-head.img out/x1slte-recovery.img <<'PY'
import sys
h = bytearray(open(sys.argv[1], 'rb').read()); d = open(sys.argv[2], 'rb').read()
assert len(h) == 69009408
vb_end = 0x1000000                      # the head image plus its vbmeta lie well inside the first 16 MiB
h[0x2000000:len(d)] = d[0x2000000:]     # the kernel and root.apfs, outside the hashed range
open(sys.argv[2], 'wb').write(h)
PY
	rm -f out/.avb-head.img
	python3 -I "$avb" info_image --image out/x1slte-recovery.img | sed -n '1,4p;/Algorithm/p;/Partition Name/p'
}

# the system volume's window: ROOT_PART=super (iOS only: super+ROOT_SUPER_OFF, ROOT_SUPER_LEN) or Android's userdata
# pinned file (ROOT_UD_OFF/LEN); sets ROOT_PART ROOT_OFF ROOT_LEN
root_window() {
	ROOT_PART=${ROOT_PART:-userdata}
	if [ "$ROOT_PART" = super ]; then
		ROOT_OFF=${ROOT_SUPER_OFF:-0}; ROOT_LEN=${ROOT_SUPER_LEN:?ROOT_SUPER_LEN unset in x1slte.conf}
		[ $((ROOT_OFF + ROOT_LEN)) -le $((SUPER_TAIL_START)) ] || die "the root window runs into super's log tail"
	else
		ROOT_OFF=${ROOT_UD_OFF:?}; ROOT_LEN=${ROOT_UD_LEN:?}
	fi
}

adb_root() {
	# TWRP: the state is "recovery" (wait-for-device would wait for Android forever) and adbd is root already
	[ "$(adb get-state 2>/dev/null)" = recovery ] && [ "$(adb shell id -u | tr -d '\r')" = 0 ] && return 0
	adb wait-for-device
	adb root > /dev/null
	sleep 2
	adb wait-for-device
}

cmd_flash() {
	adb_root
	adb push out/x1slte-recovery.img /data/local/tmp/r.img
	adb shell 'dd if=/data/local/tmp/r.img of=/dev/block/by-name/recovery bs=1M 2>/dev/null && sync'
	local size there here
	size=$(wc -c < out/x1slte-recovery.img)
	there=$(adb shell "head -c $size /dev/block/by-name/recovery | md5sum" | cut -d' ' -f1)
	here=$(md5 -q out/x1slte-recovery.img)
	[ "$there" = "$here" ] || die "verify failed: phone $there, image $here"
	echo "flash: verified ($here)"
	adb reboot recovery
}

cmd_log() {
	adb_root
	adb shell 'dd if=/dev/block/by-name/recovery of=/data/local/tmp/l.bin bs=1M skip=16 count=8 2>/dev/null'
	adb pull /data/local/tmp/l.bin ../xnulog.bin > /dev/null
	python3 -c "
d = open('../xnulog.bin', 'rb').read().replace(b'\0', b'').decode('utf-8', 'replace')   # (a NUL inside the log once hid its second half)
open('../xnulog.txt', 'w').write(d)
print(d)"
}

FASTBOOT=${FASTBOOT:-fastboot}
HEIMDALL=../heimdall-arm64/heimdall-frontend.app/Contents/MacOS/heimdall
[ -x "$HEIMDALL" ] || HEIMDALL=$HOME/ios-port-research/tools/bin/heimdall      # (the tools/build-heimdall.sh build, when the app is not there)
fb() {
	"$FASTBOOT" "$@" 2>&1 | grep -v "^Finished\.\|^Sending\|^Total time" || true
}
fb_wait() {
	local i
	for i in $(seq 1 60); do
		"$FASTBOOT" devices 2>/dev/null | grep -q fastboot && return 0
		sleep 1
	done
	die "no fastboot device: on the phone choose \"Log access (USB)\" in the menu and plug the cable in"
}
cmd_ulog() {
	fb_wait
	"$FASTBOOT" oem lastlog > /dev/null 2>&1 || die "oem lastlog failed"
	"$FASTBOOT" get_staged ../xnulog.bin > /dev/null 2>&1 || die "get_staged failed"
	python3 -c "
d = open('../xnulog.bin', 'rb').read().replace(b'\\0', b'').decode('utf-8', 'replace')
open('../xnulog.txt', 'w').write(d)
print(d)"
}
cmd_uflash() {
	local out
	fb_wait
	# in two pieces around the log (16..32 MB), so that flashing keeps the last run's log
	python3 -c "
d = open('out/x1slte-recovery.img', 'rb').read()
open('out/.uflash-head', 'wb').write(d[:0x1000000])
open('out/.uflash-tail', 'wb').write(d[0x2000000:])"
	"$FASTBOOT" stage out/.uflash-head > /dev/null 2>&1 || die "stage failed"
	out=$("$FASTBOOT" oem write recovery 0 2>&1)
	echo "$out" | grep -q "written and read back" || die "recovery (boot image): $out"
	"$FASTBOOT" stage out/.uflash-tail > /dev/null 2>&1 || die "stage failed"
	out=$("$FASTBOOT" oem write recovery 2000000 2>&1)
	echo "$out" | grep -q "written and read back" || die "recovery (kernel, root): $out"
	rm -f out/.uflash-head out/.uflash-tail
	echo "uflash: recovery written and read back by the phone, the last log kept ($(md5 -q out/x1slte-recovery.img))"
	if [ -f out/.standalone ]; then
		head -c $((0x255000)) out/x1slte-recovery.img > out/x1slte-boot.img     # (the dtb in a dt table: embed's raw FDT bricks BOOT)
		[ "$(xxd -s $((0x201800)) -l 4 -p out/x1slte-boot.img)" = d7b7ab1e ] || die "out/x1slte-boot.img: no dt table at 0x201800"
		"$FASTBOOT" stage out/x1slte-boot.img > /dev/null 2>&1 || die "stage failed"
		out=$("$FASTBOOT" oem write boot 0 2>&1)
		echo "$out" | grep -q "written and read back" || die "boot: $out"
		echo "uflash: boot written and read back"
		echo "uflash: the kernel and root are used from the next boot; the loader and its command line only after Reboot in the menu"
	fi
}
# The Metal and display lines of a log, and nothing else: rw_print's "redwoodmetal[...]" lines, the
# kernel's "mali:"/"fb:"/"dvfs:" lines, the display driver's own, and the lines this log exists for
# (slow commits, the 5 s summary, pipeline creation, pass shapes, traces).
metal_lines() {
	grep -aE 'redwoodmetal\[|(^|[0-9]\]: )(mali|fb|fb_scaler|dvfs):|MiniIOMobileFramebuffer|(^|[0-9]\]: )(slow|perf|trace|pass shape|pipeline) |slow: |perf: |pipeline [0-9]+:|pass shape:|trace: '
}
cmd_mlog() {
	if "$FASTBOOT" devices 2>/dev/null | grep -q fastboot; then
		cmd_ulog > /dev/null || die "oem lastlog failed"
	else
		cmd_wlog > /dev/null || die "no log: put the phone in Log access, or have iOS up on Wi-Fi (Y2S_IP=...)"
	fi
	metal_lines < ../xnulog.txt
}

# mali2metal's loop (PLANS/10-mali2metal.md): deploy what is built, boot it, run the standard tour, keep and
# report the Metal channel. No key press: iOS hands itself back through rwlogd's /menu (kern.redwood_menu).
# The restart after flashing is a cold one (fastboot reboot): the warm "Boot iOS" after a return does not
# start the kernel reliably (PLANS/09-standalone.md, open issue), and a loop must not depend on it.
wait_ios() {
	local host=$1 i
	for i in $(seq 1 "$2"); do
		curl -sf --max-time 4 "http://$host:8080/" > /dev/null 2>&1 && return 0
		sleep 1
	done
	return 1
}
# x1slte.cmdline with M2M_SET's key=value pairs put in (replacing the key's value, or appended): one run's
# variation of the standard line, e.g. M2M_SET="cpumask=0xff dvfs_cl2=2304000"
m2m_cmdline() {
	local line pair key
	line=" $(cat devices/exynos990/x1slte/x1slte.cmdline) "
	for pair in ${M2M_SET:-}; do
		key=${pair%%=*}
		if [[ "$line" == *" $key="* ]]; then
			line=$(printf '%s' "$line" | sed -E "s/ $key=[^ ]*/ $pair/")
		else
			line="$line$pair "
		fi
	done
	line=${line# }
	printf '%s' "${line% }"
}

cmd_loop() {
	local label=${1:-} host via=${M2M_VIA:-usb} i
	[ -f devices/exynos990/x1slte/x1slte.cmdline ] || die "no x1slte.cmdline (the standard Metal boot line)"
	host=$(phone_host)
	cmd_payload "$(m2m_cmdline)"
	cmd_pack
	if [ "$via" = download ]; then
		curl -sf --max-time 10 "http://$host:8080/menu?to=3" > /dev/null 2>&1 ||
			die "the phone did not answer on http://$host:8080/menu (is iOS up and on Wi-Fi?)"
		echo "loop: asked $host for Download Mode; waiting for the bootloader"
		sleep 12
		for i in 1 2 3 4 5 6 7 8; do
			cmd_hflash > /tmp/x1slte-hflash.log 2>&1 && break
			[ "$i" = 8 ] && { tail -3 /tmp/x1slte-hflash.log; die "the phone did not appear in Download Mode"; }
			sleep 8
		done
		echo "loop: flashed over Download Mode ($(md5 -q out/x1slte-recovery.img)); the phone restarts by itself"
	else
		if ! "$FASTBOOT" devices 2>/dev/null | grep -q fastboot; then
			curl -sf --max-time 10 "http://$host:8080/menu?to=1" > /dev/null 2>&1 ||
				die "the phone is neither in Log access nor answering on http://$host:8080 (is iOS up and on Wi-Fi?)"
			echo "loop: asked $host for Log access"
		fi
		cmd_uflash
		cmd_ureboot
	fi
	echo "loop: waiting for iOS on Wi-Fi"
	wait_ios "$host" 300 || die "iOS did not come up on Wi-Fi in 5 minutes: put the phone in Log access and ./x1slte.sh ulog"
	echo "loop: iOS is up; 45 s for SpringBoard to settle, then the tour"
	sleep 45
	# M2M_PROF=1: backboardd's sampling profiler over the whole tour (its report comes after "prof: end")
	[ -n "${M2M_PROF:-}" ] && python3 tools/m2m.py notify prof
	python3 tools/m2m.py tour
	python3 tools/m2m.py wait-tour --timeout 300 || echo "loop: the tour did not finish; the channel is kept as it is"
	if [ -n "${M2M_PROF:-}" ]; then
		python3 tools/m2m.py notify prof
		python3 tools/m2m.py wait 'prof: end' --timeout 180 || echo "loop: the profiler's report did not finish"
	fi
	python3 tools/m2m.py report "$(python3 tools/m2m.py fetch --label "$label")"
}

cmd_uroot() {
	# the iOS root image into /data/darwin/system.img through the loader (Log access): the loader takes userdata writes
	# only inside system.img's extent (USERDATA_WIN_OFF/LEN, built in by payload), and reads every chunk back.
	local f=${1:-userland/system.apfs} chunk=$((64 << 20)) size off i n out
	# shellcheck disable=SC1091
	. common/devices/x1slte.conf
	root_window
	size=$(wc -c < "$f")
	[ "$size" -le "$ROOT_LEN" ] || die "$f ($size B) is larger than the root window ($ROOT_LEN B)"
	fb_wait
	n=$(( (size + chunk - 1) / chunk ))
	for ((i = 0; i < n; i++)); do
		off=$((ROOT_OFF + i * chunk))
		dd if="$f" of=out/.uroot-chunk bs=1048576 skip=$((i * 64)) count=64 2>/dev/null
		"$FASTBOOT" stage out/.uroot-chunk > /dev/null 2>&1 || die "stage failed at chunk $i"
		out=$("$FASTBOOT" oem write "$ROOT_PART" "$(printf '%x' $off)" 2>&1)
		echo "$out" | grep -q "written and read back" || die "chunk $i of $n ($ROOT_PART+$(printf '%x' $off)): $out"
		printf '\ruroot: %d/%d chunks written and read back (%d MiB)' $((i + 1)) $n $(( (i + 1) * 64 ))
	done
	rm -f out/.uroot-chunk
	echo; echo "uroot: $f ($((size >> 20)) MiB) is in $ROOT_PART+$(printf '%x' "$ROOT_OFF")"
}

cmd_uboot() {
	# NOTE (x1slte, 2026-11-05): after Log access the loader starts the kernel with the USB device controller still
	# running, and the kernel then shows nothing (no console) -- the first run, from the menu's countdown, did. y2sios's
	# own loop restarts cold after uflash (ureboot); prefer that. Kept for when the loader stops USB before a run.
	echo "uboot: prefer ./x1slte.sh ureboot (a cold start; the menu then boots iOS)" >&2
	fb_wait
	"$FASTBOOT" oem boot > /dev/null 2>&1 || true
	echo "uboot: iOS is starting"
}
cmd_hflash() {
	if [ -f out/.standalone ]; then
		# BOOT gets the recovery image's head, whose dtb pack wrapped in a dt table: out/x1slte-embed.img's dtb is a raw
		# FDT, and S-Boot refuses it ("dt table header check fail: FDT_ERR_BADMAGIC, ufdt dtb load fail", 2026-11-06)
		head -c $((0x255000)) out/x1slte-recovery.img > out/x1slte-boot.img
		[ "$(xxd -s $((0x201800)) -l 4 -p out/x1slte-boot.img)" = d7b7ab1e ] || die "out/x1slte-boot.img: no dt table at 0x201800"
		"$HEIMDALL" flash --RECOVERY out/x1slte-recovery.img --BOOT out/x1slte-boot.img --stdout-errors
	else
		"$HEIMDALL" flash --RECOVERY out/x1slte-recovery.img --stdout-errors
	fi
}
# the boot splash Samsung's bootloader shows (up_param's logo.jpg) as the white Apple logo on black
# (out/up_param-apple.img: the backup's tar with only logo.jpg replaced). Undo: --UP_PARAM $HOME/ios-port-research/device/backup/20261105-203534/up_param.img
# a cold restart from Log access: the new loader and its command line (uflash), then the menu boots iOS by itself
cmd_ureboot() {
	fb_wait
	"$FASTBOOT" reboot > /dev/null 2>&1
	echo "ureboot: the phone restarts; the menu boots iOS after 3 s"
}
cmd_hlogo() {
	[ -f out/up_param-apple.img ] || die "no out/up_param-apple.img"
	"$HEIMDALL" flash --UP_PARAM out/up_param-apple.img --stdout-errors
}
cmd_standalone() {
	echo "This writes the HackiOS boot image into the phone's \`boot\` partition: every power-on goes to"
	echo "the HackiOS menu and Android no longer starts (its data is untouched)."
	echo "Return to Android requires full stock firmware restoration and an Android data reset; see docs/release/rollback.md"
	echo "Restoring BOOT alone cannot restore Android after super/userdata were converted to iOS."
	printf "Type yes to go on: "
	read -r answer
	[ "$answer" = yes ] || die "nothing written"
	fb_wait
	head -c $((0x255000)) out/x1slte-recovery.img > out/x1slte-boot.img     # (the dtb in a dt table: embed's raw FDT bricks BOOT)
		[ "$(xxd -s $((0x201800)) -l 4 -p out/x1slte-boot.img)" = d7b7ab1e ] || die "out/x1slte-boot.img: no dt table at 0x201800"
		"$FASTBOOT" stage out/x1slte-boot.img > /dev/null 2>&1 || die "stage failed"
	out=$("$FASTBOOT" oem write boot 0 2>&1)
	echo "$out" | grep -q "written and read back" || die "boot: $out"
	touch out/.standalone
	echo "standalone: boot written and read back; power-on now opens the menu"
}
phone_host() {
	local host
	if [ -n "${Y2S_IP:-}" ]; then
		echo "$Y2S_IP"
		return
	fi
	# the USB cable's network first (usb_ecm=1), then Wi-Fi (tools/phone-ip in ~/ios-port-research/tools)
	if command -v phone-ip > /dev/null && host=$(phone-ip 2>/dev/null); then
		echo "$host"
		return
	fi
	# Bonjour: "HackiOS._http._tcp" -> "... can be reached at NAME.local.:8080"
	host=$( (dns-sd -L HackiOS _http._tcp local & sleep 4; kill $! 2>/dev/null) 2>/dev/null |
		sed -n 's/.*can be reached at \([^:]*\)\.:8080.*/\1/p' | head -1)
	[ -n "$host" ] || die "the phone is not found over Bonjour: is iOS up and on Wi-Fi? (or set Y2S_IP)"
	echo "$host"
}
cmd_wlog() {
	local host
	host=$(phone_host)
	curl -sf --max-time 60 "http://$host:8080/log" -o ../xnulog.txt || die "no answer from http://$host:8080"
	cat ../xnulog.txt
}
cmd_wtail() {
	local host lt=0 dt=0 nl nd first=1
	host=$(phone_host)
	echo "== following http://$host:8080 (Ctrl-C ends) =="
	while :; do
		if curl -sf --max-time 20 -D /tmp/x1slte-wtail.h "http://$host:8080/tail?log=$lt&dmesg=$dt" -o /tmp/x1slte-wtail.b; then
			nl=$(sed -n 's/^X-Log-Total: \([0-9]*\).*/\1/p' /tmp/x1slte-wtail.h | tr -d '\r')
			nd=$(sed -n 's/^X-Dmesg-Total: \([0-9]*\).*/\1/p' /tmp/x1slte-wtail.h | tr -d '\r')
			if [ "$first" = 1 ]; then tail -c 4000 /tmp/x1slte-wtail.b; first=0; else cat /tmp/x1slte-wtail.b; fi
			lt=${nl:-$lt}
			dt=${nd:-$dt}
		fi
		sleep 1
	done
}
case "${1:-}" in
wlog) cmd_wlog ;;
wtail) cmd_wtail ;;
ulog) cmd_ulog ;;
mlog) cmd_mlog ;;
uflash) cmd_uflash ;;
uboot) cmd_uboot ;;
loop) cmd_loop "${2:-}" ;;
m2m) shift; python3 tools/m2m.py "$@" ;;
hflash) cmd_hflash ;;
hlogo) cmd_hlogo ;;
ureboot) cmd_ureboot ;;
standalone) cmd_standalone ;;
kernel) cmd_kernel ;;
embed) cmd_embed "${2:-}" ;;
uroot) cmd_uroot "${2:-}" ;;
payload) cmd_payload "${2:-}" ;;
cmdline) cmd_cmdline ;;
pack) cmd_pack ;;
flash) cmd_flash ;;
log) cmd_log ;;
ship) cmd_payload "${2:-}"; cmd_pack; cmd_flash ;;
*) sed -n '2,12p' "$0"; exit 1 ;;
esac
