#!/usr/bin/env python3
"""Synthetic RPC-schema checks; no network, account or proof operations."""
import base64
import importlib.util
from pathlib import Path
import secrets
spec=importlib.util.spec_from_file_location('media',Path(__file__).with_name('native-media-transport.py'))
media=importlib.util.module_from_spec(spec)
spec.loader.exec_module(media)
request={'version':1,'nonce':secrets.token_hex(16),'operation':'sign','data':base64.b64encode(b'synthetic').decode()}
assert media.valid_request(request)
for invalid in [dict(request,password='synthetic'),dict(request,operation='exec'),dict(request,data='!'),dict(request,nonce='bad'),dict(request,signature='YQ=='),dict(request,version=True)]:
    assert not media.valid_request(invalid)
assert not media.valid_request(dict(request,operation='verify'))
assert media.valid_request(dict(request,operation='verify',signature='YQ=='))
print('PASS: media RPC schema rejects extra credential fields, unknown operations, malformed input and missing signatures')

proof={'version':1,'nonce':secrets.token_hex(16),'operation':'store-proof','endpoint':'https://auth.itunes.apple.com/auth/v1/native'}
assert media.valid_request(proof)
for endpoint in ('http://auth.itunes.apple.com/auth/v1/native','https://auth.itunes.apple.com.evil.invalid/auth/v1/native','https://auth.itunes.apple.com/auth/v1/native?password=synthetic','https://auth.itunes.apple.com/auth/v1/native/unlisted'):
    assert not media.valid_request(dict(proof,endpoint=endpoint))
assert not media.valid_request(dict(proof,password='synthetic'))
print('PASS: Store-proof RPC is endpoint-only, exact HTTPS origin/path allowlisted; account/body/query fields refused')
