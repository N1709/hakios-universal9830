#!/usr/bin/env python3
"""apply_mods.py TARGET root|system DIR: copy the target's active mods into an image tree.

Called by userland/build.sh when JESVI_TARGET is set. JESVI_MODS_DISABLE lists
optional mods (devices/exynos990/mods/*) to leave out.
"""
import os
import sys
import targets

if __name__ == '__main__':
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    try:
        conf = targets.load(sys.argv[1])
        done = targets.apply(conf, sys.argv[2], sys.argv[3], os.environ.get('JESVI_MODS_DISABLE', '').split())
        print(f'mods ({sys.argv[2]}): {" ".join(done) or "none with files"}')
    except Exception as e:  # noqa: BLE001 - report and fail the image build
        sys.exit(f'apply_mods: {e}')
