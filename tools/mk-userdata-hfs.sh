#!/bin/sh
# The S20+'s data volume (PLANS/09-standalone.md, P6): a case-sensitive journaled HFS+ (HFSX) the exact size of
# its userdata partition (28382208 blocks of 4096 = 116253523968 bytes), made by macOS's own newfs_hfs on a sparse
# disk image -- nothing but files under /tmp/hfsfmt is touched. Its first 512 MB and its last 2 MB are copied out
# (head.img, tail.img); tools/hfs-kit.py takes the blocks the file system uses from them, for the phone's init
# to write into userdata at the first boot.
# SIZE (bytes, default all of userdata) makes a smaller volume for release.conf IOS_DATA_GB, e.g. SIZE=64000000000.
set -e
SIZE=${SIZE:-116253523968}
D=${D:-/tmp/hfsfmt}
mkdir -p "$D"
cd "$D"
rm -f userdata.asif head.img tail.img catalog.img
diskutil image create blank --format ASIF --size $SIZE --fs None userdata.asif
DEV=$(diskutil image attach --noMount userdata.asif | awk '{print $1}' | head -1)
case "$DEV" in /dev/disk*) ;; *) echo "attach gave no device: $DEV"; exit 1 ;; esac
echo "image attached as $DEV"
RDEV="/dev/r${DEV#/dev/}"
/sbin/newfs_hfs -J -s -b 4096 -v Data -U 0 -G 0 -M 755 "$RDEV"
dd if="$RDEV" of=head.img bs=1m count=512
dd if="$RDEV" of=tail.img bs=1m skip=$((SIZE / 1048576 - 2))
# the catalog B-tree (in the metadata zone, ~1 GB in): its first extent, from the volume header (offset 1024+288)
set -- $(dd if=head.img bs=1 skip=1312 count=8 2>/dev/null | od -An -tu1)
CAT_FIRST=$(( ($1<<24) + ($2<<16) + ($3<<8) + $4 )) CAT_BLOCKS=$(( ($5<<24) + ($6<<16) + ($7<<8) + $8 ))
dd if="$RDEV" of=catalog.img bs=4096 skip=$CAT_FIRST count=$CAT_BLOCKS
echo $SIZE > size
diskutil eject "$DEV" || hdiutil detach "$DEV"
ls -la "$D"
echo "done: $D/head.img, tail.img and catalog.img"
