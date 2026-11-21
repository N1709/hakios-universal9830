#!/usr/bin/env python3
"""Register the unmodified owner-supplied FairPlay daemon, or remove its job.

Does not patch/re-sign Apple code or provide replacement licensing responses.
System volume is untouched. Registration is for the current boot; the plist is
retained on writable storage for review/re-registration.
"""
import argparse
import hashlib
import pathlib
import plistlib
import subprocess

LABEL = 'com.apple.fairplayd.H2'
JOB = '/var/jb/Library/LaunchDaemons/com.apple.fairplayd.H2.plist'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['install', 'status', 'rollback'])
    parser.add_argument('--ssh-config', required=True)
    parser.add_argument('--binary', type=pathlib.Path)
    parser.add_argument('--original-plist', type=pathlib.Path)
    args = parser.parse_args()
    ssh = ['ssh', '-F', args.ssh_config, '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=4', 's20']
    def remote(command, check=True):
        return subprocess.run(ssh + [command], capture_output=True, check=check, timeout=15)
    if args.action == 'status':
        result = remote('/var/jb/bin/launchctl print user/501/' + LABEL, False)
        print(result.stdout.decode())
        return
    if args.action == 'rollback':
        # Only remove a job installed by this tool; never touch a native image job.
        result = remote('cat ' + JOB)
        job = plistlib.loads(result.stdout)
        if not job.get('Program', '').startswith('/var/jb/redwood/fairplay-stock/'):
            raise RuntimeError('Unexpected job; refusing rollback')
        remote('/var/jb/bin/launchctl bootout user/501/' + LABEL, False)
        remote('rm ' + JOB)
        print('Registered job removed; original binary retained.')
        return
    if not args.binary or not args.original_plist:
        parser.error('install needs original --binary and --original-plist')
    for domain in ['system', 'user/501']:
        result = remote('/var/jb/bin/launchctl print ' + domain + '/' + LABEL, False)
        if result.returncode != 113 or b'Could not find service' not in result.stderr:
            raise RuntimeError('Service already exists or lookup failed')
    if remote('test ! -e ' + JOB, False).returncode:
        raise RuntimeError('Job file already exists')
    digest = hashlib.sha256(args.binary.read_bytes()).hexdigest()
    root = '/var/jb/redwood/fairplay-stock/' + digest
    program = root + '/fairplayd.H2'
    job = plistlib.loads(args.original_plist.read_bytes())
    if job.get('Label') != LABEL or job.get('Program') != '/usr/sbin/fairplayd.H2':
        raise RuntimeError('Not the expected original job')
    job['Program'] = program
    job['KeepAlive'] = False
    # Preserve all native MachServices; no fake endpoint implementation.
    remote('mkdir -p ' + root + ' /var/jb/Library/LaunchDaemons')
    subprocess.run(['scp', '-F', args.ssh_config, '-o', 'BatchMode=yes',
                    str(args.binary), 's20:' + program], check=True, timeout=60)
    if hashlib.sha256(remote('cat ' + program).stdout).hexdigest() != digest:
        raise RuntimeError('Original binary readback mismatch')
    subprocess.run(ssh + ['cat > ' + JOB], input=plistlib.dumps(job), check=True, timeout=15)
    remote('chmod 555 ' + program + '; chmod 644 ' + JOB)
    try:
        remote('/var/jb/bin/launchctl bootstrap user/501 ' + JOB)
    except Exception:
        remote('/var/jb/bin/launchctl bootout user/501/' + LABEL, False)
        remote('rm ' + JOB, False)
        raise
    print('Registered original service on demand; SHA256=' + digest)
    print('No signing patches, account reset, KeepAlive loop or system-volume change.')

if __name__ == '__main__':
    main()
