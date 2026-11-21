#!/bin/bash
# twrp-restore.sh BACKUP.zip [PARTITION ...] — put a twrp-backup.sh backup back on an S20 that is in TWRP.
# Default: boot recovery super userdata up_param dtbo vbmeta vbmeta_samsung metadata misc cache (what any ROM or
# Jesvi iOS install changes). EFS, modem, bootloader and param are NEVER written, even if named.
# Each partition is streamed in, then read back on the phone and its SHA-256 compared with the backup's.
# Note: restoring recovery replaces TWRP; it is written last so a failure earlier still leaves TWRP.
set -euo pipefail
case "${1:-}" in -h|--help) sed -n '2,6p' "$0" | sed 's/^# \{0,1\}//'; exit 0;; esac
command -v adb >/dev/null || { echo "adb is missing: brew install android-platform-tools (or Android's platform-tools)"; exit 1; }
[ $# -ge 1 ] || { echo "usage: twrp-restore.sh BACKUP.zip [partition ...]   (--help for more)"; exit 2; }
zip=$1; shift
parts=${*:-"boot super userdata up_param dtbo vbmeta vbmeta_samsung metadata misc cache recovery"}
[ -f "$zip" ] || { echo "no such file: $zip"; exit 1; }
adb get-state 2>/dev/null | grep -q recovery || { echo "no phone in TWRP over USB (boot TWRP, plug it straight into this computer)"; exit 1; }
exec python3 -I - "$zip" $parts <<'PY'
import subprocess, sys, zipfile
NEVER = {'efs', 'sec_efs', 'cpefs', 'radio', 'cp_debug', 'param', 'bota', 'keydata', 'keyrefuge', 'keystorage',
         'persistent', 'steady', 'uh', 'uhcfg', 'spu', 'harx', 'blcmd', 'nad_refer'}
z = zipfile.ZipFile(sys.argv[1])
sums = dict(reversed(l.split('  ', 1)) for l in z.read('SHA256SUMS').decode().split('\n') if l)
parts = [p for p in sys.argv[2:] if p + '.img' in sums]
bad = [p for p in parts if p in NEVER]
if bad: sys.exit(f'refusing protected partitions: {bad}')
missing = [p for p in sys.argv[2:] if p + '.img' not in sums]
if missing: print('not in this backup, skipped:', ' '.join(missing))
print('will write:', ' '.join(parts))
if input('type RESTORE to continue: ').strip() != 'RESTORE': sys.exit('cancelled')
subprocess.run(['adb', 'shell', 'umount /data /cache /metadata 2>/dev/null; true'])
for p in parts:
    e, want = z.getinfo(p + '.img'), sums[p + '.img']
    dev = f'/dev/block/by-name/{p}'
    q = subprocess.Popen(['adb', 'exec-in', f'dd of={dev} bs=4194304 2>/dev/null; sync'], stdin=subprocess.PIPE)
    n = 0
    with z.open(e) as r:
        while c := r.read(4 << 20):
            q.stdin.write(c); n += len(c)
            print(f'\r  {p:16s} {n*100//max(e.file_size,1):3d}%', end='', flush=True)
    q.stdin.close()
    if q.wait(): sys.exit(f'\n{p}: write failed')
    have = subprocess.run(['adb', 'shell', f'head -c {e.file_size} {dev} | sha256sum'], capture_output=True, text=True).stdout.split()[0]
    print('  verified' if have == want else '  READBACK MISMATCH')
    if have != want: sys.exit(1)
print('done: adb reboot')
PY
