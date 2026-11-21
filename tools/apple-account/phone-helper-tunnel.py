#!/usr/bin/env python3
"""Maintain the two loopback-only helper forwards over pinned default S20 SSH."""
import argparse
from pathlib import Path
import signal
import subprocess
import threading

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--ssh-config',type=Path)
args=parser.parse_args()
ssh_prefix=['ssh']+(['-F',str(args.ssh_config.resolve())] if args.ssh_config else [])

stop = threading.Event()
child = None

def shutdown(*_):
    stop.set()
    if child is not None and child.poll() is None:
        child.terminate()

signal.signal(signal.SIGTERM, shutdown)
signal.signal(signal.SIGINT, shutdown)
while not stop.is_set():
    child = subprocess.Popen(ssh_prefix + [
        '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes',
        '-o', 'ConnectTimeout=4', '-o', 'ExitOnForwardFailure=yes',
        '-o', 'ServerAliveInterval=5', '-o', 'ServerAliveCountMax=2',
        '-N', '-R', '127.0.0.1:49742:127.0.0.1:49741',
        '-R', '127.0.0.1:49752:127.0.0.1:49751', 's20'])
    child.wait()
    if not stop.is_set():
        print('Helper tunnel disconnected; retrying pinned USB SSH.', flush=True)
        stop.wait(2)
