#!/usr/bin/env python3
from pathlib import Path
import hashlib
import json
import tarfile
repo = Path(__file__).resolve().parents[2]
out = repo / 'out/aod'
files = {name: out/name for name in ('RedwoodAODPreview.dylib', 'aod-preview')}
files.update({name: repo/'tools/aod'/name for name in ('RedwoodAODPreview.plist', 'install-on-phone.sh')})
files['README.md'] = repo/'docs/devices/x1slte/aod-preview.md'
manifest = dict(installed=False, device_tested=False, mode='native appearance only; hardware AOD not enabled',
                files={name: dict(bytes=p.stat().st_size, sha256=hashlib.sha256(p.read_bytes()).hexdigest())
                       for name,p in files.items()})
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
files['manifest.json'] = out/'manifest.json'
with tarfile.open(out/'preview-bundle.tar.gz','w:gz') as bundle:
    for name,path in files.items():bundle.add(path,arcname=name,recursive=False)
print(out/'preview-bundle.tar.gz')

# Separate persistent bundle; no enabled preferences or private device data.
persistent = {name: out/name for name in ('RedwoodAlwaysOn.dylib', 'aod-preview')}
persistent.update({name: repo/'tools/aod'/name for name in ('RedwoodAlwaysOn.plist', 'install-alwayson-on-phone.sh')})
persistent['README.md'] = repo/'docs/devices/x1slte/aod-preview.md'
release = dict(default_enabled=False, mode='native AOD with live composition; panel ALPM not implemented',
               files={name: dict(bytes=p.stat().st_size, sha256=hashlib.sha256(p.read_bytes()).hexdigest())
                      for name,p in persistent.items()})
(out/'alwayson-manifest.json').write_text(json.dumps(release,indent=2)+'\n')
persistent['manifest.json'] = out/'alwayson-manifest.json'
with tarfile.open(out/'alwayson-bundle.tar.gz','w:gz') as bundle:
    for name,path in persistent.items():bundle.add(path,arcname=name,recursive=False)
print(out/'alwayson-bundle.tar.gz')
