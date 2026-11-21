#!/usr/bin/env python3
"""Loopback broker for a strictly scoped native machine-data helper over SSH.

No account-login endpoint, arbitrary signing input, headers/proofs in logs,
account credentials, keychain access or public listener. Native proof bytes
exist only in the helper/SSH/broker/phone memory, never written to artifacts.
"""
import argparse
import importlib.util
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import select
import subprocess
import threading
import time
import plistlib
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

FIELDS = {'X-Apple-I-MD', 'X-Apple-I-MD-M', 'X-Apple-I-MD-RINFO', 'X-Apple-I-MD-LU',
          'X-Apple-I-Client-Time', 'X-Mme-Device-Id', 'X-MMe-Client-Info',
          'X-Apple-I-SRL-NO', 'X-Mme-Nas-Qualify', 'User-Agent'}


def valid_signing_metadata(data):
    if not isinstance(data, bytes) or not 0 < len(data) <= 16384:
        return False
    try:
        root = plistlib.loads(data)
        if (not isinstance(root, dict) or set(root) != {'protocolVersion', 'userInfo', 'delegates'}
                or root['protocolVersion'] != '1.0'):
            return False
        user, delegates = root['userInfo'], root['delegates']
        if not isinstance(user, dict) or set(user) != {'client-id', 'language', 'timezone'}:
            return False
        if not isinstance(user['client-id'], str) or len(user['client-id']) != 36:
            return False
        uuid.UUID(user['client-id'])
        if (not isinstance(user['language'], str) or len(user['language']) > 35
                or not re.fullmatch(r'[A-Za-z]{2,8}(?:-[A-Za-z0-9]{1,8}){0,3}', user['language'])
                or not isinstance(user['timezone'], str) or len(user['timezone']) > 64
                or not re.fullmatch(r'[A-Za-z0-9_+./:-]+', user['timezone'])):
            return False
        if not isinstance(delegates, dict) or len(delegates) > 3:
            return False
        for key, value in delegates.items():
            if not isinstance(value, dict):
                return False
            if key in ('com.apple.mobileme', 'com.apple.gamecenter'):
                if value:
                    return False
            elif key == 'com.apple.private.ids':
                if set(value) != {'protocol-version'} or value['protocol-version'] not in ('1', '2', '3', '4'):
                    return False
            else:
                return False
        return True
    except Exception:
        return False


class NativeTransport:
    def __init__(self, host, key, known_hosts, helper='hackintosh-provider-v5', ssh_config=None):
        if not re.fullmatch(r'[A-Za-z0-9_.-]+@[A-Za-z0-9_.-]+', host):
            raise ValueError('Invalid SSH destination')
        if not re.fullmatch(r'[A-Za-z0-9_-]+', helper):
            raise ValueError('Invalid helper filename')
        self.command = [
            'ssh', '-i', str(key), '-o', 'IdentitiesOnly=yes', '-o', 'BatchMode=yes',
            '-o', 'StrictHostKeyChecking=yes', '-o', 'UserKnownHostsFile=' + str(known_hosts),
            '-o', 'ConnectTimeout=5', '-o', 'ServerAliveInterval=15',
            '-o', 'ServerAliveCountMax=2', host,
            '.local/share/redwood-account-lab/' + helper + ' --stdio 2>/dev/null',
        ]
        if ssh_config is not None:
            self.command[1:1] = ['-F', str(Path(ssh_config).resolve())]
        self.process = self._spawn()
        self.lock = threading.Lock()

    def _spawn(self):
        return subprocess.Popen(self.command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)

    def fetch(self, nonce, timeout=4, signing_data=None, service=None):
        if service not in (None, "icloud") or (service and signing_data is not None):
            raise ValueError("Invalid service context")
        with self.lock:
            if self.process.poll() is not None:
                self.process = self._spawn()
            message = {'version': 1, 'nonce': nonce}
            if service:message['service'] = service
            if signing_data is not None:
                if not valid_signing_metadata(signing_data):
                    raise ValueError('Invalid signing metadata')
                message['icloud_body'] = base64.b64encode(signing_data).decode()
            request = json.dumps(message, separators=(',', ':')).encode() + b'\n'
            self.process.stdin.write(request)
            output = bytearray()
            end = time.monotonic() + timeout
            while b'\n' not in output:
                remaining = end - time.monotonic()
                if remaining <= 0 or not select.select([self.process.stdout], [], [], remaining)[0]:
                    # A late line must never be mistaken for the next request.
                    self.close()
                    raise TimeoutError('Native transport timed out')
                chunk = os.read(self.process.stdout.fileno(), 65536)
                if not chunk:
                    raise RuntimeError('Native transport closed')
                output.extend(chunk)
                if len(output) > 65536:
                    self.close()
                    raise ValueError('Oversized helper reply')
            reply = json.loads(output)
            now = time.time()
            if (not isinstance(reply, dict) or reply.get('version') != 1 or reply.get('nonce') != nonce
                    or not isinstance(reply.get('issued_at'), (float, int))
                    or not isinstance(reply.get('expires_at'), (float, int))
                    or abs(reply['issued_at'] - now) > 10 or reply['expires_at'] <= now
                    or not 0 < reply['expires_at'] - reply['issued_at'] <= 30):
                raise ValueError('Invalid helper envelope')
            headers = reply.get('headers')
            if signing_data is not None and reply.get('signed_metadata_sha256') != hashlib.sha256(signing_data).hexdigest():
                raise ValueError('Signing input mismatch')
            if (not isinstance(headers, dict) or set(headers) != FIELDS
                    or any(not isinstance(value, str) or not 0 < len(value) <= 16384
                           or '\n' in value or '\r' in value for value in headers.values())
                    or not headers['X-Apple-I-MD-RINFO'].isdigit()):
                raise ValueError('Invalid helper metadata')
            return reply

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host', required=True)
    p.add_argument('--ssh-config', type=Path)
    p.add_argument('--media-helper',help='Enable owner-native media signing helper')
    p.add_argument('--helper', default='hackintosh-provider-v5')
    p.add_argument('--key', type=Path, required=True)
    p.add_argument('--known-hosts', type=Path, required=True)
    p.add_argument('--token-file', type=Path, required=True)
    p.add_argument('--port', type=int, default=49741)
    args = p.parse_args()
    os.umask(0o077)
    token = args.token_file.read_text().strip()
    if not re.fullmatch(r'[A-Za-z0-9_-]{32,128}', token):
        raise ValueError('Invalid broker token')
    transport = NativeTransport(args.host, args.key.resolve(), args.known_hosts.resolve(), helper=args.helper, ssh_config=args.ssh_config)
    media = None
    media_module = None
    if args.media_helper:
        if not re.fullmatch(r'native-media-provider-v[0-9]+',args.media_helper):raise ValueError('Invalid media helper')
        spec=importlib.util.spec_from_file_location('native_media_transport',Path(__file__).with_name('native-media-transport.py'))
        media_module=importlib.util.module_from_spec(spec);spec.loader.exec_module(media_module)
        command=transport.command.copy()
        command[-1]='.local/share/redwood-account-lab/'+args.media_helper+' --stdio 2>/dev/null'
        media=media_module.MediaTransport(command)
    try:
        try:
            transport.fetch(secrets.token_hex(16), timeout=30)  # Optional warm-up; recover on next request.
        except Exception:
            transport.close()
        latest_signing = [None, 0.0, None]
        signing_lock = threading.Lock()
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *unused):
                pass

            def reply(self, status, value):
                body = json.dumps(value, separators=(',', ':')).encode()
                self.send_response(status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Cache-Control', 'no-store')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path not in ('/v1/recent-signing-metadata', '/v1/fresh-icloud-metadata'):
                    self.reply(404, {'error': 'not_found'}); return
                if not secrets.compare_digest(self.headers.get('Authorization', ''), 'Bearer ' + token):
                    self.reply(401, {'error': 'unauthorized'}); return
                if self.path == '/v1/fresh-icloud-metadata':
                    try:
                        reply = transport.fetch(secrets.token_hex(16), service='icloud')
                        headers = {k:v for k,v in reply['headers'].items() if k not in ('X-Mme-Nas-Qualify','User-Agent')}
                        self.reply(200, {'issued_at':reply['issued_at'], 'expires_at':reply['expires_at'], 'machine_headers':headers})
                    except Exception:
                        self.reply(503, {'error':'native_provider_unavailable'})
                    return
                with signing_lock:
                    body, issued, snapshot = latest_signing
                if body is None or time.time()-issued > 90:
                    self.reply(404, {'error': 'no_fresh_metadata'}); return
                self.reply(200, {'issued_at': issued, 'icloud_body': base64.b64encode(body).decode(), 'machine_headers': snapshot})

            def do_POST(self):
                if self.path == '/v1/media':
                    if not secrets.compare_digest(self.headers.get('Authorization',''),'Bearer '+token):
                        self.reply(401,{'error':'unauthorized'});return
                    if media is None:
                        self.reply(503,{'error':'media_provider_unavailable'});return
                    try:
                        length=int(self.headers.get('Content-Length','0'))
                        if not 0<length<=370000:raise ValueError()
                        value=json.loads(self.rfile.read(length))
                        if not media_module.valid_request(value):raise ValueError()
                    except (ValueError,TypeError):
                        self.reply(400,{'error':'invalid_request'});return
                    try:self.reply(200,media.fetch(value))
                    except Exception:self.reply(503,{'error':'media_provider_unavailable'})
                    return
                if self.path != '/v1/anisette':
                    self.reply(404, {'error': 'not_found'}); return
                if not secrets.compare_digest(self.headers.get('Authorization', ''), 'Bearer ' + token):
                    self.reply(401, {'error': 'unauthorized'}); return
                try:
                    length = int(self.headers.get('Content-Length', '0'))
                    if not 0 < length <= 24000:
                        raise ValueError()
                    request = json.loads(self.rfile.read(length))
                    if (not isinstance(request, dict) or set(request) not in ({'version', 'nonce'}, {'version', 'nonce', 'icloud_body'})
                            or type(request['version']) is not int or request['version'] != 1
                            or not isinstance(request['nonce'], str)
                            or not re.fullmatch(r'[A-Za-z0-9_-]{16,64}', request['nonce'])):
                        raise ValueError()
                    signing_data = None
                    if 'icloud_body' in request:
                        if not isinstance(request['icloud_body'], str) or len(request['icloud_body']) > 22000:
                            raise ValueError()
                        signing_data = base64.b64decode(request['icloud_body'], validate=True)
                        if not valid_signing_metadata(signing_data):
                            raise ValueError()
                except (ValueError, TypeError, KeyError):
                    self.reply(400, {'error': 'invalid_request'}); return
                try:
                    reply = transport.fetch(request['nonce'], signing_data=signing_data)
                    if signing_data is not None:
                        with signing_lock:
                            latest_signing[:] = [signing_data, time.time(), reply['headers']]
                    self.reply(200, reply)
                except Exception:
                    self.reply(503, {'error': 'native_provider_unavailable'})

        server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
        server.daemon_threads = True
        print(json.dumps({'status': 'ready', 'listen': '127.0.0.1', 'port': args.port,
                          'provider': 'native-hackintosh', 'account_login_attempted': False}), flush=True)
        try:
            server.serve_forever()
        finally:
            server.server_close()
    finally:
        if media is not None:media.close()
        transport.close()


if __name__ == '__main__':
    main()
