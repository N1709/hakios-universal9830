import hashlib
import json
from pathlib import Path
import sys
import tempfile
import subprocess
import unittest
import zipfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from restore_audit import audit_package
from twrp_package import device_config
from manage import ROOT, profile


class RestoreAuditTests(unittest.TestCase):
    def make(self, root, change=None, extra=False):
        path = root / 'restore_preflight.zip'
        config = device_config(profile('x1slte'), 'restore', 'restore', False)
        if change:
            change(config)
        elf = bytearray(64)
        elf[:6] = b'\x7fELF\x02\x01'
        elf[18:20] = b'\xb7\x00'
        with zipfile.ZipFile(path, 'w') as z:
            z.writestr('installer.json', json.dumps(config))
            z.writestr('README.txt', 'HARDWARE QUALIFICATION PENDING')
            z.writestr('META-INF/com/google/android/update-binary', elf)
            z.writestr('META-INF/com/google/android/updater-script', '# native')
            if extra:
                z.writestr('firmware.bin', b'private firmware')
        sha = hashlib.sha256(path.read_bytes()).hexdigest()
        Path(str(path) + '.sha256').write_text(f'{sha}  {path.name}\n')
        Path(str(path) + '.build.json').write_text(json.dumps(dict(zip_sha256=sha, action='restore',
            source_dirty=False, writes_enabled=False, hardware_qualified=False, device='x1slte', source_commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip())))
        return path

    def test_exact_preflight_inventory(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertFalse(audit_package(self.make(Path(d)))['writes_enabled'])

    def test_rejects_extra_firmware_even_with_valid_checksums(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaisesRegex(ValueError, 'inventory'):
                audit_package(self.make(Path(d), extra=True))

    def test_rejects_enabled_writes_or_unexpected_partition(self):
        for change in (lambda c: c.update(allow_writes=True),
                       lambda c: c['partitions'].update(efs=1024)):
            with tempfile.TemporaryDirectory() as d:
                with self.assertRaisesRegex(ValueError, 'config'):
                    audit_package(self.make(Path(d), change))

    def test_rejects_modified_zip(self):
        with tempfile.TemporaryDirectory() as d:
            path = self.make(Path(d))
            path.write_bytes(path.read_bytes() + b'tamper')
            with self.assertRaisesRegex(ValueError, 'checksum'):
                audit_package(path)
