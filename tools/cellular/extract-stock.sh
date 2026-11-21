#!/bin/sh
# Rebuild the stock partitions from the local firmware (private, gitignored): super.img -> system, vendor, product (ext4)
set -e
# needs: python3 -m venv out/cellular/tools/venv && out/cellular/tools/venv/bin/pip install lz4
P=$(cd "$(dirname "$0")/../.." && pwd)/out/cellular/private-rom
"$(cd "$(dirname "$0")/../.." && pwd)/out/cellular/tools/venv/bin/python" "$(dirname "$0")/ap2super.py" "$HOME/Desktop/SM-G980F_INU_G980FXXSNHYB7/AP_G980FXXSNHYB7_G980FXXSNHYB7_MQB93262781_REV01_user_low_ship_MULTI_CERT_meta_OS13.tar.md5" "$P/super.img"
dd if="$P/super.img" of="$P/stock-system.img" bs=1m skip=1 count=5882
dd if="$P/super.img" of="$P/stock-vendor.img" bs=1m skip=$((0x175a00000/1048576)) count=1369
dd if="$P/super.img" of="$P/stock-product.img" bs=1m skip=$((0x1cca00000/1048576)) count=1186
rm -f "$P/super.img"
ls -la "$P"
echo extract-stock done
