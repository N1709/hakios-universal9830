"""Verify rule surgery preserves unrelated and concurrently added configuration."""
import importlib.util
from pathlib import Path
s=importlib.util.spec_from_file_location('layer',Path(__file__).with_name('shared-layer.py'));m=importlib.util.module_from_spec(s);s.loader.exec_module(m)
old='/var/tmp/anisette-lab-bridge-v14.dylib akd'
foreign='/redwood/lib/graphics.dylib /Applications/'
metal='/var/jb/usr/lib/libredwood-metaloff.dylib Preferences'
current=';'.join([foreign,old,metal])
updated,removed,rule=m.plan_rules(current,'/var/jb/usr/lib/libredwood-account-layer-0123456789abcdef.dylib')
assert removed==[old] and foreign in updated and metal in updated
concurrent='/var/jb/usr/lib/new-unrelated.dylib Camera'
restored=m.rollback_rules(updated+';'+concurrent,{'installed_rule':rule,'removed_rules':removed})
assert set(restored.split(';'))==set([foreign,old,metal,concurrent])
try:m.plan_rules('/var/tmp/anisette-lab-bridge-v14.dylib securityd','/x')
except ValueError:pass
else:raise AssertionError('Unexpected privileged process accepted')
print('PASS: install and rollback preserve graphics, Metal policy and concurrent unrelated rules; unexpected process refused')
