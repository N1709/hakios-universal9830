#!/usr/bin/env python3
"""Probe an isolated macOS ADI emulator; no account credentials or header output.

The Apple binary stays outside git. --provision contacts Apple's provisioning
endpoints once for a newly generated virtual identity. It does not sign in.
"""
import argparse
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import urllib.parse

REVIEWED_COMMIT = 'd4d2a832baf47d1cf43d989879950ac2aec45f47'


def write_private(path, value):
    temporary = path.with_suffix(path.suffix + '.new')
    fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')
    temporary.replace(path)


def entry_address(data):
    if data[:4] != bytes.fromhex('cffaedfe'):
        raise ValueError('Expected a thin little-endian 64-bit Mach-O')
    if struct.unpack_from('<I', data, 4)[0] != 0x1000007:
        raise ValueError('Expected an x86_64 slice')
    off, entry, text = 32, None, None
    for _ in range(struct.unpack_from('<I', data, 16)[0]):
        cmd, size = struct.unpack_from('<II', data, off)
        if size < 8 or off + size > len(data):
            raise ValueError('Invalid Mach-O load command')
        if cmd == 0x80000028:
            entry = struct.unpack_from('<Q', data, off + 8)[0]
        if cmd == 0x19 and data[off + 8:off + 24].rstrip(b'\0') == b'__TEXT':
            text = (struct.unpack_from('<Q', data, off + 24)[0],
                    struct.unpack_from('<Q', data, off + 40)[0])
        off += size
    if entry is None or text is None:
        raise ValueError('Missing entry point or text segment')
    return text[0] + entry - text[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--upstream', type=Path, required=True)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--state', type=Path, required=True)
    p.add_argument('--provision', action='store_true')
    args = p.parse_args()
    os.umask(0o077)
    commit = subprocess.check_output(
        ['git', '-C', str(args.upstream), 'rev-parse', 'HEAD'], text=True).strip()
    if commit != REVIEWED_COMMIT:
        raise ValueError('Upstream checkout differs from reviewed commit')
    package = args.upstream / 'macOS-ADI-emulator/src'
    sys.path.insert(0, str(package.resolve()))
    from macos_adi_emulator import adi, provision
    from macos_adi_emulator.gsa import GSA, Device
    from macos_adi_emulator.identity import mint_identity

    binary = args.binary.read_bytes()
    adi.MAIN = entry_address(binary)
    state = args.state.resolve()
    state.mkdir(mode=0o700, parents=True, exist_ok=True)
    state.chmod(0o700)
    identity_file = state / 'identity.json'
    identity = json.loads(identity_file.read_text()) if identity_file.exists() else mint_identity()
    write_private(identity_file, identity)
    report = {'account_login_attempted': False, 'network_requested': args.provision,
              'binary_sha256': hashlib.sha256(binary).hexdigest(),
              'upstream_commit': commit, 'stage': 'boot'}
    try:
        # Suppress third-party diagnostic output, including generated identifiers.
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            daemon = adi.Adid(args.binary.resolve(), host_uuid=bytes.fromhex(identity['host_uuid']))
            # Static code-signing role read from Apple's akd executable metadata.
            # This describes a virtual caller, not the host's device identity.
            daemon._client['adi_client'] = '347420559'
            provision.load_store(daemon, state)
            report['stage'] = 'status'
            ready = daemon.is_machine_provisioned(-2)
            if not ready and args.provision:
                marker = state / 'provision-attempt.json'
                if marker.exists():
                    raise RuntimeError('A prior attempt is recorded; inspect its result before retrying')
                write_private(marker, {'attempt_started': True})
                server = GSA(Device(identity['device_id'], identity['client_info'],
                                    identity['local_user_uuid'], identity.get('serial')))
                original_open = server._open

                def apple_only(url, body, *, client_time):
                    parsed = urllib.parse.urlparse(url)
                    host = parsed.hostname or ''
                    if parsed.scheme != 'https' or not host.endswith('.apple.com'):
                        raise ValueError('Refusing a provisioning endpoint outside Apple HTTPS')
                    return original_open(url, body, client_time=client_time)

                server._open = apple_only
                report['stage'] = 'url_bag'
                server.load_urlbag()
                report['stage'] = 'start_provisioning'
                spim = server.start_provisioning()
                report['stage'] = 'local_provisioning'
                cpim, session = daemon.start_provisioning(-2, spim)
                report['stage'] = 'finish_provisioning'
                finished = server.finish_provisioning(cpim)
                report['stage'] = 'save_provisioning'
                daemon.end_provisioning(session, finished['ptm'], finished['tk'])
                if finished['routing']:
                    daemon.set_idms_routing(-2, finished['routing'])
                identity['routing'] = finished['routing']
                write_private(identity_file, identity)
                provision.save_store(daemon, state)
                ready = daemon.is_machine_provisioned(-2)
            report['provisioned'] = ready
            if ready:
                report['stage'] = 'machine_proof'
                mid, proof = daemon.request_otp(-2)
                report['machine_id_nonempty'] = bool(mid)
                report['machine_proof_nonempty'] = bool(proof)
                provision.save_store(daemon, state)
            report['stage'] = 'complete'
    except Exception as error:
        # Do not render exception messages: upstream errors may include server bodies.
        report['error_type'] = type(error).__name__
        code = getattr(error, 'code', None)
        if isinstance(code, int):
            report['error_code'] = code
    write_private(state / 'result.json', report)
    print(json.dumps(report, indent=2))
    return 0 if report.get('stage') == 'complete' else 1


if __name__ == '__main__':
    raise SystemExit(main())
