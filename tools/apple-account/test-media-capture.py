#!/usr/bin/env python3
"""Response-summary privacy/protocol checks; no network or real credentials."""
import importlib.util
import json
from pathlib import Path
import plistlib
spec=importlib.util.spec_from_file_location('capture',Path(__file__).with_name('media-capture.py'))
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
fixture={'Response':{'Status':{'ec':-22406,'em':'review your account'},'tokens':{'secret':'NEVER_PRINT_TOKEN'},'account':'fixture@example.invalid'}}
for data in (plistlib.dumps(fixture),json.dumps(fixture).encode()):
 summary=module.response_summary(data)
 assert summary['numeric_status']['Response.Status.ec']==-22406
 assert summary['categories']==['account_review']
 assert 'NEVER_PRINT_TOKEN' not in json.dumps(summary) and '@example.invalid' not in json.dumps(summary)
assert module.response_summary(b'<html>synthetic</html>')['format']=='html'
assert module.response_summary(b'\x00\xff')['format']=='binary'
print('PASS: nested protocol errors summarized without token/account/message values; plist, JSON, HTML and binary distinguished')
