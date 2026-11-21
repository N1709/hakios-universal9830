#!/usr/bin/env python3
"""One privileged FINGE research exchange over SSH. Never prints payload bytes."""
import argparse
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import time
from wire import decode_reply, validate_request

MARKER = 'fingerprint wire: exchange-v2 (research only), active 0'
SSH = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', 's20']
STATUS = '/var/jb/usr/sbin/sysctl kern.redwood.tee'


def require_idle(status):
    if MARKER not in status:
        raise ValueError('Research exchange-v2 kernel is absent or a fingerprint command is active')


def private_input(path):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or stat.S_IMODE(info.st_mode) != 0o600 or info.st_nlink != 1:
        raise ValueError('Request must be a regular, single-link mode-0600 file')
    return path.read_bytes()


def remote(command, data=None, timeout=15):
    result = subprocess.run(SSH + [command], input=data, capture_output=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError('SSH operation failed (status %d); payload/stderr withheld' % result.returncode)
    return result.stdout


def _exchange(request_path, reply_path):
    packet = private_input(request_path)
    command = validate_request(packet)
    if stat.S_IMODE(reply_path.parent.stat().st_mode) != 0o700:
        raise ValueError('Reply parent directory must have mode 0700')
    # Reserve the local destination before changing phone state; never overwrite an earlier reply.
    fd = os.open(reply_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    finished = False
    try:
        require_idle(remote(STATUS).decode('utf-8', 'replace'))
        remote('umask 077; mkdir -p /var/root/fp-research; chmod 700 /var/root/fp-research; '
               'cat > /var/root/fp-research/request.bin; chmod 600 /var/root/fp-research/request.bin', packet)
        remote('/var/jb/usr/sbin/sysctl -w kern.redwood.tee="fp exchange" >/dev/null')
        deadline = time.monotonic() + 60
        while True:
            status = remote(STATUS).decode('utf-8', 'replace')
            done = re.search(r'last: fp exchange: done rc (\d+) transport (?:0x[0-9a-f]+|0) origin \d+ cmd \d+', status)
            if done and MARKER in status:
                if int(done.group(1)):
                    raise RuntimeError('Kernel research exchange failed with errno ' + done.group(1))
                break
            if time.monotonic() >= deadline:
                raise TimeoutError('Research exchange still pending; do not overlap another command')
            time.sleep(0.5)
        blob = remote('cat /var/root/fp-research/reply.bin')
        summary, _ = decode_reply(blob, command)
        with os.fdopen(fd, 'wb') as output:
            fd = -1
            output.write(blob)
            output.flush()
            os.fsync(output.fileno())
        finished = True
        return summary
    finally:
        if fd >= 0:
            os.close(fd)
        if not finished:
            reply_path.unlink()


@contextmanager
def session_lock(directory=None):
    # Shared across all checkouts on this Mac, unlike a per-worktree lock.
    directory = directory or (Path.home() / '.cache' / 'jesvi-fingerprint')
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    info = directory.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.geteuid() or stat.S_IMODE(info.st_mode) != 0o700:
        raise ValueError('Unsafe fingerprint lock directory')
    fd = os.open(directory / 'exchange.lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or info.st_nlink != 1 or stat.S_IMODE(info.st_mode) != 0o600:
            raise ValueError('Unsafe fingerprint lock file')
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        os.close(fd)


def exchange(request_path, reply_path):
    with session_lock():
        return _exchange(request_path, reply_path)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('request', type=Path)
    parser.add_argument('reply', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(exchange(args.request, args.reply), sort_keys=True))
    except (ValueError, OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        parser.exit(1, str(error) + '\n')
