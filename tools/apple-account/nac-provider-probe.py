#!/usr/bin/env python3
"""Offline NAC emulator setup with a synthetic profile, no account or network.

Reviewed pypush revision: 6336bba82697b838497ffd774f23e5640c877a85.
Never use upstream data.plist or generatenac.py: the former is a shared profile,
the latter prints proof data. This probe does not generate a validation proof.
Requires the existing private environment's unicorn package.
"""
import argparse
import hashlib
import importlib
import json
import logging
import os
from pathlib import Path
import plistlib
import shutil
import sys
import types
import uuid
import ssl
import urllib.request
import urllib.parse
import re

SOURCE_HASHES = {
    'nac.py': '4690fadf1d46820d92fa28929478d5201bceb905820d07f8bc6d3a35efc343fc',
    'jelly.py': '8335f45e9971566e9803a14270e65aa7a1b8db083456930f17ca467e155cbeb8',
    'mparser.py': 'c5365a6e225a9c69235e06aaad73e7b4a00c21292b4be3894c072607e7b4354c',
}
BINARY_HASH = '74c2a8fe826a478f14f6e17b3709a8b315e8c0e0e34e3fba6b9c4eee2f4516e9'


def synthetic_profile(identity):
    host = uuid.UUID(hex=identity['host_uuid'])
    def derived(role):
        return hashlib.sha256(host.bytes + role.encode()).digest()
    mac = b'\x02' + derived('network')[:5]
    board = derived('board-serial').hex().upper()[:17].encode()
    iokit = {
        'IOPlatformSerialNumber': identity['serial'],
        'IOPlatformUUID': str(host).upper(),
        'IOMACAddress': mac,
        'board-id': b'Mac-66E35819EE2D0D05\0',
        'product-name': b'MacBookPro13,2\0',
        '4D1EDE05-38C7-4A6A-9CC6-4BCCA8B38C14:MLB': board,
        '4D1EDE05-38C7-4A6A-9CC6-4BCCA8B38C14:ROM': mac,
    }
    for role in ['Fyp98tpgj', 'Gq3489ugfi', 'abKPld1EcMni', 'kbjfrfpoJU', 'oycqAZloTNDm']:
        iokit[role] = derived(role)[:17]
    return {'iokit': iokit, 'root_disk_uuid': str(uuid.uuid5(host, 'lab-root-disk')).upper()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sources', type=Path, required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--adi-identity', type=Path, required=True)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--online-once', action='store_true',
                        help='One credential-free validation handshake; no login or proof output')
    args = parser.parse_args()
    os.umask(0o077)
    logging.disable(logging.CRITICAL)
    sources = args.sources.resolve()
    binary = args.binary.read_bytes()
    if hashlib.sha256(binary).hexdigest() != BINARY_HASH:
        parser.exit(1, 'Refused: unexpected generic binary\n')
    for name, digest in SOURCE_HASHES.items():
        if hashlib.sha256((sources / name).read_bytes()).hexdigest() != digest:
            parser.exit(1, 'Refused: unexpected reviewed source\n')
    identity = json.loads(args.adi_identity.read_text())
    profile = synthetic_profile(identity)
    runtime = args.runtime.resolve()
    package = runtime / 'emulated'
    package.mkdir(parents=True, exist_ok=True, mode=0o700)
    (package / '__init__.py').write_text('')
    for name in SOURCE_HASHES:
        shutil.copyfile(sources / name, package / name)
    (package / 'data.plist').write_bytes(plistlib.dumps(profile))
    def forbidden(*unused, **kwargs):
        raise RuntimeError('Network disabled in offline probe')
    requests = types.ModuleType('requests')
    requests.get = requests.post = forbidden
    sys.modules['requests'] = requests
    os.chdir(runtime)
    sys.path.insert(0, str(runtime))
    report = {'network_requests': 0, 'account_login_attempted': False,
              'donor_profile_used': False, 'proof_generated': False}
    try:
        nac = importlib.import_module('emulated.nac')
        nac.load_binary = lambda: binary
        emulator = nac.load_nac()
        report['offline_setup'] = True
        if args.online_once:
            attempt = runtime / 'online-attempt.json'
            if attempt.exists():
                raise ValueError('Already attempted')
            attempt.write_text(json.dumps({'attempted': True}) + '\n')
            import certifi
            import unicorn
            context = ssl.create_default_context(cafile=certifi.where())
            def fetch(url, payload=None):
                host = urllib.parse.urlsplit(url).hostname
                if host not in {'static.ess.apple.com', 'identity.ess.apple.com'}:
                    raise ValueError('Unexpected host')
                request = urllib.request.Request(url, data=payload,
                    headers={'Content-Type': 'application/x-apple-plist'})
                report['network_requests'] += 1
                with urllib.request.urlopen(request, timeout=15, context=context) as response:
                    final = urllib.parse.urlsplit(response.geturl())
                    if final.scheme != 'https' or final.hostname != host:
                        raise ValueError('Unexpected redirect')
                    data = response.read(262145)
                    if len(data) > 262144:
                        raise ValueError('Oversized reply')
                return plistlib.loads(data)
            # The upstream engine has no instruction/time limit: add both.
            def bounded_call(self, address, args=()):
                self.push(0x00900000)
                self._set_args(args)
                self.uc.emu_start(address, 0x00900000, timeout=10000000, count=20000000)
                if self.uc.reg_read(unicorn.x86_const.UC_X86_REG_RIP) != 0x00900000:
                    raise TimeoutError()
                return self.uc.reg_read(unicorn.x86_const.UC_X86_REG_RAX)
            type(emulator.instr).call = bounded_call
            report['stage'] = 'certificate'
            certificate = fetch('https://static.ess.apple.com/identity/validation/cert-1.0.plist')['cert']
            report['stage'] = 'native_initialization'
            validation_context, challenge = nac.nac_init(emulator, certificate)
            report['stage'] = 'validation_handshake'
            reply = fetch('https://identity.ess.apple.com/WebObjects/TDIdentityService.woa/wa/initializeValidation',
                          plistlib.dumps({'session-info-request': bytes(challenge)}))
            report['stage'] = 'key_establishment'
            nac.nac_key_establishment(emulator, validation_context, reply['session-info'])
            report['stage'] = 'proof_generation'
            proof = nac.nac_sign(emulator, validation_context)
            report['proof_generated'] = bool(proof)
            report['stage'] = 'complete'
    except Exception as error:
        # No arbitrary exception text: upstream errors may embed data.
        report.setdefault('offline_setup', False)
        report['error_type'] = type(error).__name__
        match = re.fullmatch(r'Error calling (nac_init|nac_submit|nac_generate): (-?\d+)', str(error))
        if match:
            report['native_error_code'] = int(match[2])
    print(json.dumps(report, indent=2))
    return 0 if report['offline_setup'] and 'error_type' not in report else 1


if __name__ == '__main__':
    raise SystemExit(main())
