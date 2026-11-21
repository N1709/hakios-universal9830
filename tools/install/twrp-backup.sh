#!/bin/bash
# twrp-backup.sh [DEST_DIR] — full backup of a Galaxy S20 that is in TWRP, into ONE zip on the computer.
# Every partition (boot, recovery, super, userdata, up_param, dtbo, vbmeta, efs, modem, ...) plus the raw
# partition tables, read with dd over the USB cable. Nothing on the phone is written. Each image is stored
# compressed (deflate, level 1) inside the zip with its SHA-256 in SHA256SUMS, checked while it streams.
# Restore: tools/install/twrp-restore.sh ZIP (never writes EFS, modem or bootloader partitions).
# Default folder: /Volumes/Jesvi SSD/BACKUPS/S20 when that drive is in, else ~/S20-backups.
# SKIP="userdata super" leaves partitions out. Needs adb and python3.
set -euo pipefail
case "${1:-}" in -h|--help) sed -n '2,7p' "$0" | sed 's/^# \{0,1\}//'; exit 0;; esac
command -v adb >/dev/null || { echo "adb is missing: brew install android-platform-tools (or Android's platform-tools)"; exit 1; }
dflt="$HOME/S20-backups"; [ -d "/Volumes/Jesvi SSD" ] && dflt="/Volumes/Jesvi SSD/BACKUPS/S20"
dest=${1:-$dflt}
adb get-state 2>/dev/null | grep -q recovery || { echo "no phone in TWRP over USB (boot TWRP, plug it straight into this computer)"; exit 1; }
mkdir -p "$dest"
model=$(adb shell getprop ro.product.model | tr -d '\r')
out="$dest/S20-full-backup_${model}_$(date +%Y%m%d-%H%M%S).zip"
echo "backing up $model into $out"
exec python3 -I - "$out" "${SKIP:-}" <<'PY'
import hashlib, json, os, shutil, subprocess, sys, time, zipfile
out, skip = sys.argv[1], set(sys.argv[2].split())
sh = lambda c: subprocess.run(['adb', 'shell', c], capture_output=True, text=True, check=True).stdout.replace('\r', '')
names = sorted(n for n in sh('ls /dev/block/by-name').split()   # TWRP also lists whole disks (sda: 119 GiB) there
               if n not in skip and not (len(n) == 3 and n.startswith('sd')))
size = {n: int(sh(f'blockdev --getsize64 /dev/block/by-name/{n}').strip()) for n in names}
disks = [d for d in ('sda', 'sdb', 'sdc', 'sdd', 'sde', 'sdf') if sh(f'[ -b /dev/block/{d} ] && echo y; true').strip() == 'y']
total = sum(size.values())
free = shutil.disk_usage(os.path.dirname(out)).free
print(f'{len(names)} partitions, {total/2**30:.1f} GiB raw; {free/2**30:.0f} GiB free here')
if free < total * 0.6:
    print('warning: may not fit if the data partition is full (compressed size is unknown until read)')
sums, info, done, t0 = [], {}, 0, time.time()
def pull(z, entry, cmd, want):
    global done
    h, n = hashlib.sha256(), 0
    p = subprocess.Popen(['adb', 'exec-out', cmd], stdout=subprocess.PIPE)
    with z.open(entry, 'w', force_zip64=True) as w:
        while chunk := p.stdout.read(4 << 20):
            h.update(chunk); w.write(chunk); n += len(chunk); done += len(chunk)
            el = time.time() - t0
            print(f'\r  {entry:28s} {n*100//max(want,1):3d}%   all {done*100//max(total,1):3d}%  {done/2**20/max(el,1):5.0f} MB/s', end='', flush=True)
    if p.wait() or n != want:
        raise SystemExit(f'\n{entry}: read {n} of {want} bytes; backup INCOMPLETE')
    print()
    return h.hexdigest()
with zipfile.ZipFile(out + '.partial', 'w', zipfile.ZIP_DEFLATED, compresslevel=1, allowZip64=True) as z:
    total += len(disks) << 20
    for d in disks:  # the partition tables: the first 1 MiB of each disk, plus sgdisk's listing
        sums.append(f'{pull(z, f"gpt/{d}-head.bin", f"dd if=/dev/block/{d} bs=1048576 count=1 2>/dev/null", 1 << 20)}  gpt/{d}-head.bin')
        info[d] = sh(f'sgdisk --print /dev/block/{d} 2>/dev/null || cat /proc/partitions')
    for n in names:
        sums.append(f'{pull(z, n + ".img", f"dd if=/dev/block/by-name/{n} bs=4194304 2>/dev/null", size[n])}  {n}.img')
    z.writestr('SHA256SUMS', '\n'.join(sums) + '\n')
    z.writestr('manifest.json', json.dumps({'created': time.strftime('%Y-%m-%d %H:%M:%S'),
        'model': sh('getprop ro.product.model').strip(), 'bootloader': sh('getprop ro.boot.bootloader').strip(),
        'sizes': size, 'skipped': sorted(skip), 'tables': info}, indent=1))
os.rename(out + '.partial', out)
print(f'done: {out} ({os.path.getsize(out)/2**30:.1f} GiB, {(time.time()-t0)/60:.0f} min)')
PY
