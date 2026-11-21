#!/bin/bash
# push-iphone8-cache.sh: put the iPhone 8's iOS 16.5 (20F66) dyld shared cache on the phone's data volume, for Touch ID.
# biometrickitd's Touch ID job (userland/com.apple.biometrickitd.mesa.plist) runs on this cache, where Apple's
# libBKDM1 (the Touch ID sensor module) lives; nothing else uses it. Apple's file, from Apple's CDN
# (sysimage/fetch-from-ipsw.py with the iPhone 8 IPSW, unpacked with 7z + unzbm); never in the repository.
# Needs ssh s20 (a development image). Resumable: files already there with the right SHA-256 are skipped.
set -euo pipefail
SRC=${1:-$(cd "$(dirname "$0")/../.." && pwd)/sysimage/iphone8/dsc/System/Library/Caches/com.apple.dyld}
DST=/private/var/redwood/iphone8-dsc
cd "$SRC"
files=$(ls dyld_shared_cache_arm64 dyld_shared_cache_arm64.[0-9]* 2>/dev/null)
[ -n "$files" ] || { echo "no cache in $SRC"; exit 1; }
ssh s20 "mkdir -p $DST && chmod 755 /private/var/redwood $DST"
remote=$(ssh s20 "cd $DST && for f in *; do [ -f \"\$f\" ] && /var/jb/usr/bin/sha256sum \"\$f\" 2>/dev/null || true; done" || true)
total=0; n=0
for f in $files; do
	want=$(shasum -a 256 "$f" | cut -d' ' -f1)
	if echo "$remote" | grep -q "^$want  $f\$"; then continue; fi
	size=$(stat -f %z "$f"); total=$((total + size)); n=$((n + 1))
	echo "$(date +%T) $f ($((size / 1048576)) MB)"
	ssh -C s20 "cat > $DST/.$f.part && mv $DST/.$f.part $DST/$f && chmod 644 $DST/$f" < "$f"
	got=$(ssh s20 "/var/jb/usr/bin/sha256sum $DST/$f" | cut -d' ' -f1)
	[ "$got" = "$want" ] || { echo "$f: checksum differs on the phone"; exit 1; }
done
echo "done: $n files, $((total / 1048576)) MB sent; cache at $DST"
