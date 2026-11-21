#!/usr/bin/env python3
"""Loopback-only broker for an already provisioned, isolated ADI identity.

Accepts only a version and random nonce. Never accepts account credentials,
contacts Apple, provisions, or prints machine proof values. Use an SSH tunnel
for the phone; do not bind this service to a network interface.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--upstream', type=Path, required=True)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--binary-sha256', required=True)
    p.add_argument('--state', type=Path, required=True)
    p.add_argument('--token-file', type=Path, required=True)
    p.add_argument('--port', type=int, default=49731)
    args = p.parse_args()
    os.umask(0o077)
    spec = importlib.util.spec_from_file_location('provider_probe',
        Path(__file__).with_name('emulated-provider-probe.py'))
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)
    commit = subprocess.check_output(['git', '-C', str(args.upstream),
                                     'rev-parse', 'HEAD'], text=True).strip()
    if commit != probe.REVIEWED_COMMIT:
        raise ValueError('Unexpected upstream revision')
    dirty = subprocess.check_output(['git', '-C', str(args.upstream),
                                     'status', '--porcelain', '-uno'], text=True)
    if dirty:
        raise ValueError('Upstream tracked source differs from reviewed revision')
    binary = args.binary.read_bytes()
    if hashlib.sha256(binary).hexdigest() != args.binary_sha256:
        raise ValueError('Unexpected Apple binary hash')
    sys.path.insert(0, str((args.upstream / 'macOS-ADI-emulator/src').resolve()))
    from macos_adi_emulator import adi, provision
    adi.MAIN = probe.entry_address(binary)
    state = args.state.resolve()
    identity = json.loads((state / 'identity.json').read_text())
    daemon = adi.Adid(args.binary.resolve(), host_uuid=bytes.fromhex(identity['host_uuid']))
    daemon._client['adi_client'] = '347420559'
    provision.load_store(daemon, state)
    if not daemon.is_machine_provisioned(-2):
        raise ValueError('Identity has not been provisioned')
    if args.token_file.exists():
        token = args.token_file.read_text().strip()
    else:
        token = secrets.token_urlsafe(32)
        fd = os.open(args.token_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, 'w') as stream:
            stream.write(token + '\n')
    if not re.fullmatch(r'[A-Za-z0-9_-]{32,128}', token):
        raise ValueError('Invalid broker token')
    lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *unused):
            pass

        def send_json(self, status, value):
            body = json.dumps(value, separators=(',', ':')).encode()
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            if self.path != '/v1/anisette':
                self.send_json(404, {'error': 'not_found'})
                return
            supplied = self.headers.get('Authorization', '')
            if not secrets.compare_digest(supplied, 'Bearer ' + token):
                self.send_json(401, {'error': 'unauthorized'})
                return
            try:
                length = int(self.headers.get('Content-Length', '0'))
                if not 0 < length <= 256:
                    raise ValueError()
                request = json.loads(self.rfile.read(length))
                if (not isinstance(request, dict) or set(request) != {'version', 'nonce'}
                    or request['version'] != 1 or not isinstance(request['nonce'], str)
                    or not re.fullmatch(r'[A-Za-z0-9_-]{16,64}', request['nonce'])):
                    raise ValueError()
            except (ValueError, TypeError, KeyError):
                self.send_json(400, {'error': 'invalid_request'})
                return
            try:
                with lock:
                    headers = provision.headers_from(daemon, identity, identity['routing'])
                    if not headers.get('X-Apple-I-MD') or not headers.get('X-Apple-I-MD-M'):
                        raise ValueError()
                    provision.save_store(daemon, state)
                now = int(time.time())
                self.send_json(200, {'version': 1, 'nonce': request['nonce'],
                    'issued_at': now, 'expires_at': now + 25, 'headers': headers})
            except Exception:
                self.send_json(503, {'error': 'provider_unavailable'})

    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    server.daemon_threads = True
    print(json.dumps({'status': 'ready', 'listen': '127.0.0.1',
                      'port': server.server_address[1], 'account_login_attempted': False}), flush=True)
    try:
        server.serve_forever()
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
