#!/usr/bin/env python3
"""Inventory publication risks without printing matched secrets or rewriting history.

This is a triage tool, not a substitute for a full-history secret/license audit.
Strict mode blocks release creation until all reported files have been reviewed
and removed from the release branch or explicitly handled in a follow-up change.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BINARY_SUFFIXES = {'.img', '.apfs', '.bin', '.dtb', '.efi', '.fd', '.dylib', '.o', '.a', '.jpg', '.png'}
REVIEWED = ROOT / 'tools/release/publication-reviewed.json'
SECRET = re.compile(rb'-----BEGIN (?:OPENSSH |RSA |EC )?PRIVATE KEY-----|gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,}')


def reviewed():
    """Files a person reviewed and decided to publish, by content: a changed file is reviewed again."""
    try:
        return {k: v['sha256'] for k, v in json.loads(REVIEWED.read_text())['files'].items() if v.get('decision') == 'publish'}
    except FileNotFoundError:
        return {}


def audit():
    ok = reviewed()
    skipped = 0
    names = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0')
    findings = []
    for name in filter(None, names):
        p = ROOT / name
        if not p.is_file() or p.is_symlink():
            continue
        data = p.read_bytes()
        reasons = []
        if p.suffix.lower() in BINARY_SUFFIXES or b'\x00' in data[:8192]:
            reasons.append('binary/artifact provenance review')
        if name.startswith(('prebuilt/', 'docs/research/')):
            reasons.append('research/prebuilt publication review')
        if SECRET.search(data):
            reasons.append('possible credential; value redacted')
        if name.endswith(('.orig', '.rej', '.log')):
            reasons.append('generated/debug file')
        if reasons and 'possible credential; value redacted' not in reasons and ok.get(name) == hashlib.sha256(data).hexdigest():
            skipped += 1
            continue
        if reasons:
            findings.append({'path': name, 'bytes': len(data), 'reasons': reasons})
    return {'schema': 1, 'scope': 'tracked working tree only; full-history scan still required',
            'findings': findings, 'count': len(findings), 'reviewed_and_unchanged': skipped}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--strict', action='store_true')
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    result = audit()
    text = json.dumps(result, indent=2) + '\n'
    if a.output:
        a.output.parent.mkdir(parents=True, exist_ok=True)
        a.output.write_text(text)
        print(f'{result["count"]} publication-review items; report: {a.output}')
    else:
        print(text, end='')
    sys.exit(1 if a.strict and result['count'] else 0)
