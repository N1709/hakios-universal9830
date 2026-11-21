#!/usr/bin/env python3
"""Package only explicit UI binaries/configuration; exclude all private data."""
from pathlib import Path
import hashlib
import json
import tarfile

repo = Path(__file__).resolve().parents[3]
source = Path(__file__).resolve().parent
out = repo / 'out/fingerprint/ui'
files = {name: out / name for name in ('redwood-fingerprint-ui', 'libredwood-fingerprint-ui.dylib')}
files.update({name: source / name for name in ('com.redwood.fingerprint-ui.plist',
    'RedwoodFingerprintPreview.plist', 'install-on-phone.sh', 'README.md')})
manifest = dict(installed=False, device_tested=False, authentication='unavailable',
                files={name: dict(bytes=p.stat().st_size, sha256=hashlib.sha256(p.read_bytes()).hexdigest())
                       for name, p in files.items()})
(out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
files['manifest.json'] = out / 'manifest.json'
archive = out.parent / 'fingerprint-ui-preview.tar.gz'
with tarfile.open(archive, 'w:gz') as bundle:
    for name, path in files.items():
        bundle.add(path, arcname='fingerprint-ui-preview/' + name, recursive=False)
print(archive)
