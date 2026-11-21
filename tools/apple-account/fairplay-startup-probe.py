#!/usr/bin/env python3
"""Bounded stock-daemon startup check; no signing patches or account calls.

Input directory contains the original extracted usr/sbin/fairplayd.H2.
The original service must be absent. The transient job is always unloaded.
"""
import hashlib
import pathlib
import plistlib
import subprocess
import sys
import time

def main():
    config, extracted, original_plist = map(pathlib.Path, sys.argv[1:])
    ssh = ['ssh', '-F', str(config), '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=4', 's20']
    def remote(command, check=True):
        return subprocess.run(ssh + [command], text=True, capture_output=True,
                              timeout=12, check=check)
    for domain in ('system', 'user/501'):
        result = remote(f'/var/jb/bin/launchctl print {domain}/com.apple.fairplayd.H2', False)
        if result.returncode != 113 or 'Could not find service' not in result.stderr:
            raise RuntimeError('Original service exists or baseline lookup failed')
    binary = extracted / 'usr/sbin/fairplayd.H2'
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    destination = '/var/tmp/account-fairplay-' + digest[:16]
    job = plistlib.loads(original_plist.read_bytes())
    job['Program'] = destination + '/fairplayd.H2'
    job['KeepAlive'] = False
    local_job = extracted / 'one-shot.plist'
    local_job.write_bytes(plistlib.dumps(job))
    remote(f'mkdir -m 755 {destination}')
    subprocess.run(['scp', '-F', str(config), '-o', 'BatchMode=yes', str(binary),
                    str(local_job), 's20:' + destination + '/'], check=True, timeout=60)
    remote(f'chmod 555 {destination}/fairplayd.H2; chmod 644 {destination}/one-shot.plist')
    try:
        remote(f'/var/jb/bin/launchctl bootstrap user/501 {destination}/one-shot.plist')
        remote('/var/jb/bin/launchctl kickstart user/501/com.apple.fairplayd.H2', False)
        time.sleep(2)
        result = remote('/var/jb/bin/launchctl print user/501/com.apple.fairplayd.H2', False)
        for line in result.stdout.splitlines():
            if any(field in line for field in ('state =', 'runs =', 'pid =', 'last exit code =')):
                print(line.strip())
    finally:
        remote('/var/jb/bin/launchctl bootout user/501/com.apple.fairplayd.H2', False)
        result = remote('/var/jb/bin/launchctl print user/501/com.apple.fairplayd.H2', False)
        if 'Could not find service' not in result.stderr:
            raise RuntimeError('Verify transient job cleanup manually')
        print('Transient job unloaded; original account services unchanged.')

if __name__ == '__main__':
    main()
