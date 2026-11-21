#!/usr/bin/env python3
"""Install/remove only the Cellular Settings tweak, while holding the phone lock.

Does not acquire/steal a lock, flash, reboot, or restart a system service.
Close and reopen Settings after install/removal. Existing copies are preserved.
"""
import argparse
import datetime
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]
RESEARCH = ROOT.parent.parent
REMOTE = '/var/jb/usr/lib/TweakInject'


def run(command):
    subprocess.run(command, check=True, timeout=30)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['install', 'remove'])
    parser.add_argument('--session', default='codex-cellular')
    parser.add_argument('--host', default='s20')
    args = parser.parse_args()
    owner_file = RESEARCH / 'build/phone.lock/owner'
    owner = owner_file.read_text().split()[0] if owner_file.exists() else None
    if owner != args.session:
        parser.exit(3, f'No changes made: phone lock is {owner or "free"}; '
                       f'{args.session} must acquire it through tools/phone-lock.sh first.\n')
    if args.host.startswith('-'):
        parser.error('invalid SSH host')
    ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', args.host]
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    backup = REMOTE + '/RedwoodCellular-backup-' + stamp
    if args.action == 'remove':
        run(ssh + ['set -e; mkdir -p ' + shlex.quote(backup) + '; '
                   'for ext in dylib plist; do p=' + REMOTE + '/RedwoodCellular.$ext; '
                   'if test -f "$p"; then mv "$p" ' + shlex.quote(backup) + '/; fi; done'])
    else:
        artifacts = ROOT / 'out/cellular'
        for ext in ('dylib', 'plist'):
            if not (artifacts / ('RedwoodCellular.' + ext)).is_file():
                parser.exit(1, 'Build first: bash tools/cellular/build-ui.sh\n')
        run(ssh + ['test -d ' + REMOTE + ' && test -f /redwood/lib/libredwood-tweaks.dylib'])
        # Upload under inert names, then rename; never overwrite a mapped dylib.
        for ext in ('dylib', 'plist'):
            run(['scp', '-q', '-O', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5',
                 str(artifacts / ('RedwoodCellular.' + ext)),
                 args.host + ':' + REMOTE + '/RedwoodCellular.' + stamp + '.' + ext + '.new'])
        commands = ['set -e', 'mkdir -p ' + shlex.quote(backup)]
        for ext in ('dylib', 'plist'):
            dst = REMOTE + '/RedwoodCellular.' + ext
            tmp = REMOTE + '/RedwoodCellular.' + stamp + '.' + ext + '.new'
            commands += [f'if test -f {dst}; then cp -p {dst} {backup}/; fi',
                         f'chmod 644 {tmp}', f'mv -f {tmp} {dst}']
        run(ssh + ['; '.join(commands)])
    print('Cellular UI ' + args.action + ' complete. Close and reopen Settings.')
    print('Previous files, if any: ' + backup)


if __name__ == '__main__':
    main()
