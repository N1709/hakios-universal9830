#!/usr/bin/env python3
"""Read-only phone preflight. Never starts TEE, copies files or changes state."""
import json
import subprocess
import sys

REMOTE = r'''
/var/jb/usr/sbin/sysctl kern.redwood.tee
for p in /var/root/teetsd /var/root/tee/driver /var/root/fp-calib/cbge_0.dat /var/root/fp-calib/cbge_8.dat /var/root/fp-template.bin /var/jb/bin/launchctl /var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodSoftwarePasscode.dylib; do
  if test -e "$p"; then printf 'PRESENT %s\n' "$p"; fi
done
'''

def inspect(text):
    paths = {line[8:] for line in text.splitlines() if line.startswith('PRESENT ')}
    required = {'storage_daemon': '/var/root/teetsd', 'driver_tas': '/var/root/tee/driver',
                'factory_slot_0': '/var/root/fp-calib/cbge_0.dat',
                'factory_slot_8': '/var/root/fp-calib/cbge_8.dat',
                'launchctl': '/var/jb/bin/launchctl',
                'passcode_relock_tweak': '/var/jb/Library/MobileSubstrate/DynamicLibraries/RedwoodSoftwarePasscode.dylib'}
    result = dict(read_only=True,
                  compatible_kernel='fingerprint ui: reject-v1 (no authentication)' in text,
                  tee_started='TEEGRIS: up;' in text,
                  kernel_idle='exchange-v2 (research only), active 0' in text,
                  saved_template_present='/var/root/fp-template.bin' in paths,
                  assets={key: path in paths for key, path in required.items()},
                  authentication='unavailable')
    result['ready_for_backend_test'] = (result['compatible_kernel'] and result['tee_started']
                                      and result['kernel_idle'] and all(result['assets'].values()))
    return result


def main():
    try:
        process = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=4', 's20', REMOTE],
                                 capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        print('Phone preflight unavailable; nothing changed.', file=sys.stderr)
        return 1
    if process.returncode:
        print('Phone preflight failed; nothing changed.', file=sys.stderr)
        return 1
    result = inspect(process.stdout)
    print(json.dumps(result, indent=2))
    return 0 if result['ready_for_backend_test'] else 2


if __name__ == '__main__':
    sys.exit(main())
