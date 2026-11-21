#!/usr/bin/env python3
"""Report upstream movement without pulling into, merging or rewriting this tree."""
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
lock = json.loads((root / 'ci/sources.lock.json').read_text())
lock['sources'].update(json.loads((root / 'ci/upstream-watch.json').read_text())['sources'])
rows = []
for name, source in sorted(lock['sources'].items()):
    result = subprocess.run(['git', 'ls-remote', source['url'], source['ref'], source['ref'] + '^{}'],
                            text=True, capture_output=True, timeout=90)
    refs = dict((line.split()[1], line.split()[0]) for line in result.stdout.splitlines())
    latest = refs.get(source['ref'] + '^{}', refs.get(source['ref']))
    rows.append({'source': name, 'locked': source['commit'], 'upstream': latest,
                 'status': 'unavailable' if result.returncode or not latest else
                 'unchanged' if latest == source['commit'] else 'review update'})
print(json.dumps(rows, indent=2))
