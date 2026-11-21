#!/usr/bin/env python3
"""Check clean S20 assembly without requiring private iOS/firmware assets."""
from pathlib import Path
import os
import plistlib
import subprocess
import tempfile

repo = Path(__file__).resolve().parents[2]
def config(device=None, override=None):
    env = os.environ.copy()
    env.pop('DEVICE', None)
    env.pop('SOFTWARE_PASSCODE', None)
    if device is not None: env['DEVICE'] = device
    if override is not None: env['SOFTWARE_PASSCODE'] = override
    return subprocess.run(['bash', '-c', 'source ./software-passcode-config.sh; printf "%s" "$SOFTWARE_PASSCODE"'],
                          cwd=repo/'userland', env=env, capture_output=True, text=True)
for device in (None, 'x1slte'):
    r=config(device); assert r.returncode == 0 and r.stdout == '1'
r=config('redwood'); assert r.returncode == 0 and r.stdout == '0'
r=config('x1slte', '0'); assert r.returncode == 0 and r.stdout == '0'
assert config('x1slte', 'invalid').returncode != 0
line=(repo/'devices/exynos990/x1slte/x1slte.cmdline').read_text().strip()
assert 'softpasscode=1' in line.split() and 'autounlock=0' in line.split()
line=line.replace('@ROOT_PART@','super').replace('@ROOT_OFF@','0').replace('@ROOT_LEN@','100000000')
assert len(line+' bt_addr=00:00:00:00:00:00') <= 550
assert b'immediate-lock policy registered' in (repo/'prebuilt/userland/libredwood-sb.dylib').read_bytes()
assert b'UI startup refused' in (repo/'prebuilt/userland/softkeybag-init').read_bytes()
with tempfile.TemporaryDirectory(prefix='swkb-assembly-') as tmp:
    tree=Path(tmp)
    (tree/'etc').mkdir(); (tree/'LaunchDaemons').mkdir()
    (tree/'etc/rc').write_text('# test startup\nexec /sbin/launchd.ios\n')
    for _ in range(2):
        subprocess.run(['python3',str(repo/'tools/passcode/prepare-root.py'),str(tree),
                        '--binaries',str(repo/'prebuilt/userland')],check=True,capture_output=True)
    rc=(tree/'etc/rc').read_text()
    assert rc.count('/redwood/bin/softkeybag-init') == 1
    assert 'exec /redwood/bin/softkeybag-init /sbin/launchd.ios' in rc
    job=plistlib.loads((tree/'LaunchDaemons/com.apple.mobile.keybagd.plist').read_bytes())
    assert job['Label'] == 'com.apple.mobile.keybagd' and job['Umask'] == 0o77
    assert 'OS_ACTIVITY_DT_MODE' not in job.get('EnvironmentVariables',{})
    for source,target in [('softkeybag-init','bin/softkeybag-init'),('libredwood-sb.dylib','lib/libredwood-sb.dylib')]:
        assert (tree/target).read_bytes() == (repo/'prebuilt/userland'/source).read_bytes()
print('PASS: S20 defaults, other-device/explicit overrides, boot-line budget, prebuilt policy and idempotent clean root assembly')
