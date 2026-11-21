#!/usr/bin/env python3
"""Native macOS login comparison. Keep fixed categories/codes, never raw log text.
Runs locally on the Mac being observed. Does not authenticate or change accounts.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import re
import select
import subprocess
import time

PHASES = {
    'pet_obtained': ('successfully obtained a new pet',),
    'pet_missing': ('successful srp did not return a pet',),
    'srp_failed': ('srp authentication with server failed',),
    'password_auth_selected': ('using password auth',),
    'basic_token_auth_selected': ('using token auth (basic)',),
    'account_not_provisioned': ('account not provisioned',),
    'provisioning_failed': ('adiotprequest failed',),
    'raw_password_result_selected': ('providing raw password to an internal client',),
    'short_lived_token_selected': ('providing short-lived token',),
    'service_token_fetch_succeeded': ('fetching tokens for service ids',),
}
REQUESTS = ('AALoginAccountRequest', 'AALoginDelegatesRequest', 'AAAuthenticateRequest', 'AARegisterRequest', 'AKSRPRequest')
DOMAINS = ('AKAuthenticationError', 'AKAuthenticationServerError', 'com.apple.AppleIDAuthSupport', 'com.apple.appleaccount', 'com.apple.AppleAccount.Error', 'NSCocoaErrorDomain', 'NSURLErrorDomain')
ENDPOINTS = ('/setup/signin/v2/login', '/setup/iosbuddy/loginDelegates', '/grandslam/GsService2')


def summarize(row):
    if not isinstance(row, dict):
        return None
    text = row.get('eventMessage')
    if not isinstance(text, str) or len(text) > 65536:
        return None
    lower = text.lower()
    phases = [name for name, needles in PHASES.items() if any(needle in lower for needle in needles)]
    # This phrase alone describes an attempt; require the success suffix.
    if 'service_token_fetch_succeeded' in phases and 'succeeded' not in lower:
        phases.remove('service_token_fetch_succeeded')
    requests = [name for name in REQUESTS if name in text]
    endpoints = [name for name in ENDPOINTS if name in text]
    errors = []
    for domain in DOMAINS:
        for match in re.finditer(r'Domain=' + re.escape(domain) + r'\s+Code=(-?[0-9]{1,8})(?![0-9])', text):
            errors.append({'domain': domain, 'code': int(match.group(1))})
    statuses = [int(m.group(1)) for m in re.finditer(r'\bHTTP(?:/\d(?:\.\d)?)?\s+(?:status(?:\s+code)?\s*[=:]?\s*)?(200|201|204|400|401|403|404|429|500|502|503)\b', text, re.I)]
    if not (phases or requests or endpoints or errors or statuses):
        return None
    return {'event': 'native_login_summary', 'phases': phases, 'request_types': requests,
            'known_endpoints': endpoints, 'errors': errors, 'http_statuses': statuses}


def selftest():
    text = 'AALoginAccountRequest HTTP status=401 Error Domain=AKAuthenticationError Code=-7006 password=synthetic-password token=synthetic-token account=private@example.invalid'
    result = summarize({'eventMessage': text})
    encoded = json.dumps(result)
    assert result['http_statuses'] == [401] and result['errors'][0]['code'] == -7006
    assert all(v not in encoded for v in ('synthetic-password', 'synthetic-token', 'private@example.invalid'))
    assert summarize({'eventMessage': 'password=synthetic-password token=synthetic-token'}) is None
    assert summarize({'eventMessage': 'Fetching tokens for service IDs private'}) is None
    assert summarize({'eventMessage': 'Fetching tokens for service IDs private succeeded!'})['phases'] == ['service_token_fetch_succeeded']
    print('PASS: fixed native login summaries exclude raw text, account identifiers and secrets')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--system-log', action='store_true', help='Use existing noninteractive admin access for log visibility')
    ap.add_argument('--duration', type=int, default=600)
    ap.add_argument('--output', type=Path)
    args = ap.parse_args()
    if args.self_test:
        selftest()
        return
    if args.output is None or not 1 <= args.duration <= 1800:
        ap.error('Need output and duration1..1800')
    # Never enable private-data logging or persist the raw child stream.
    predicate = '(process == "akd" OR process == "accountsd" OR process == "System Settings" OR process == "AppleIDSettings" OR subsystem BEGINSWITH "com.apple.appleaccount" OR subsystem BEGINSWITH "com.apple.authkit")'
    command = ['/usr/bin/log', 'stream', '--style', 'ndjson', '--level', 'debug', '--predicate', predicate]
    if args.system_log:
        command = ['/usr/bin/sudo', '-n'] + command
    child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, 'O_NOFOLLOW', 0), 0o600)
    count = 0
    started = time.monotonic()
    try:
        with os.fdopen(fd, 'w', buffering=1) as output:
            output.write(json.dumps({'event': 'collector_started', 'pid': os.getpid(), 'duration_seconds': args.duration}) + '\n')
            pending = b''
            while time.monotonic() - started < args.duration and child.poll() is None:
                if not select.select([child.stdout], [], [], 1)[0]:
                    continue
                chunk = os.read(child.stdout.fileno(), 8192)
                if not chunk:
                    break
                pending += chunk
                if len(pending) > 1048576:
                    pending = b''
                    continue
                while b'\n' in pending:
                    line, pending = pending.split(b'\n', 1)
                    try:
                        result = summarize(json.loads(line))
                    except (ValueError, UnicodeError):
                        continue
                    if result:
                        result['captured_at'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
                        output.write(json.dumps(result, separators=(',', ':')) + '\n')
                        count += 1
            output.write(json.dumps({'event': 'collector_finished', 'summary_count': count, 'child_exit_code': child.poll()}) + '\n')
    finally:
        child.terminate()
        try:
            child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
    print(json.dumps({'event': 'collector_completed', 'summary_count': count}))

if __name__ == '__main__':
    main()
