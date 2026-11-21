#!/usr/bin/env python3
"""Choose ORIGINAL or EXPERIMENTAL front worker for the next Camera stream.
Requires the dual-profile development library. Does not restart Camera or reboot.
Rear selection is independent. Experimental is NOT verified Samsung factory tuning.
"""
import argparse
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('profile', choices=['status','original','experimental'])
a = p.parse_args()
flag = '/var/mobile/Library/Caches/com.apple.mediaserverd/redwood-front-profile'
if a.profile == 'status':
    command = f'if test -f {flag}; then cat {flag}; else echo original; fi'
    subprocess.run(['ssh','-o','BatchMode=yes','s20',command], check=True)
else:
    command = (f'test -f /var/redwood/camera-profile-build.json && '
               f'cat > {flag}.new && chmod 644 {flag}.new && mv {flag}.new {flag}')
    subprocess.run(['ssh','-o','BatchMode=yes','s20',command], input=(a.profile+'\n').encode(), check=True)
    print(f'Front profile {a.profile} selected for the next stream. Close/reopen Camera; rear is unchanged.')
