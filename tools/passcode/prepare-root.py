#!/usr/bin/env python3
"""Opt-in software-passcode additions to an existing small userland tree."""
import argparse
from pathlib import Path
import shutil
import plistlib
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('tree',type=Path)
ap.add_argument('--binaries',type=Path,default=Path('prebuilt/userland'))
a=ap.parse_args();repo=Path(__file__).resolve().parents[2]
rc=a.tree/'etc/rc';text=rc.read_text()
for old in ('exec /sbin/launchd.ios','exec /sbin/launchd'):
 if old in text:
  text=text.replace(old,'exec /redwood/bin/softkeybag-init '+old[5:]);break
else:
 if 'exec /redwood/bin/softkeybag-init ' not in text:raise SystemExit('No supported launchd handoff in rc')
for src,dest in [('softkeybag-init','bin/softkeybag-init'),('libredwood-sb.dylib','lib/libredwood-sb.dylib')]:
 s=a.binaries/src
 if not s.is_file():raise SystemExit('Missing '+str(s))
 (a.tree/dest).parent.mkdir(parents=True,exist_ok=True)
 shutil.copy2(s,a.tree/dest)
rc.write_text(text)
x=plistlib.loads((repo/'userland/com.redwood.keybagd.plist').read_bytes())
(a.tree/'LaunchDaemons/com.apple.mobile.keybagd.plist').write_bytes(plistlib.dumps(x,sort_keys=False))
print('Prepared software passcode restore, guarded UI helper, and native keybagd job')
