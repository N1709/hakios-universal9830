import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import local_build

class LocalBuildTests(unittest.TestCase):
    def test_explicit_tar_build_reuses_verified_extraction(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = root / 'dist/candidate'
            commands = []
            def run(args, **kwargs):
                commands.append(args)
                if 'package' in args:
                    output.mkdir(parents=True)
                    for name in ('boot.img','recovery.img','system.apfs','manifest.json','SHA256SUMS'):
                        (output/name).write_bytes(b'fixture')
                return type('Result', (), {'returncode': 0})()
            with patch.object(local_build, 'ROOT', root), patch.object(local_build, 'preflight'), \
                 patch.object(local_build, 'verify_extraction') as verify, \
                 patch.object(local_build.subprocess, 'run', side_effect=run):
                local_build.build(output, twrp=False)
            verify.assert_called_once_with(root)
            self.assertFalse(any('sysimage/fetch.sh' in args for args in commands))
            self.assertEqual([a[1] for a in commands], ['identity','lock-inputs','images','package','verify'])
            with tarfile.open(str(output)+'.tar') as tar:
                self.assertEqual(len(tar.getmembers()), 5)
            self.assertTrue(Path(str(output)+'.tar.sha256').is_file())

    def test_failed_build_never_packages_or_reports_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = root / 'dist/candidate'
            commands = []
            def run(args, **kwargs):
                commands.append(args)
                return type('Result', (), {'returncode': 1 if 'images' in args else 0})()
            with patch.object(local_build, 'ROOT', root), patch.object(local_build, 'preflight'), \
                 patch.object(local_build, 'verify_extraction'), \
                 patch.object(local_build.subprocess, 'run', side_effect=run):
                with self.assertRaisesRegex(ValueError, '03-build.*failed'):
                    local_build.build(output)
            self.assertFalse(any('package' in a for a in commands))
            self.assertFalse(Path(str(output)+'.tar').exists())

class RecoveryOutputTests(unittest.TestCase):
    def test_twrp_pipeline_omits_duplicate_tar_and_propagates_explicit_write_opt_in(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = root / 'dist/candidate'
            commands = []
            def run(args, **kwargs):
                commands.append(args)
                if 'package' in args:
                    output.mkdir(parents=True)
                return type('Result', (), {'returncode': 0})()
            with patch.object(local_build, 'ROOT', root), patch.object(local_build, 'preflight'), \
                 patch.object(local_build, 'verify_extraction'), \
                 patch.object(local_build.subprocess, 'run', side_effect=run):
                local_build.build(output, twrp=True, experimental=True)
            self.assertEqual(commands[-1], ['./build.sh','twrp','--bundle',str(output),'--output',str(output)+'.zip','--experimental'])
            self.assertFalse(Path(str(output)+'.tar').exists())

    def test_default_build_emits_preflight_zip_without_enabling_writes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            output = root / 'dist/candidate'
            commands = []
            def run(args, **kwargs):
                commands.append(args)
                if 'package' in args:
                    output.mkdir(parents=True)
                return type('Result', (), {'returncode': 0})()
            with patch.object(local_build, 'ROOT', root), patch.object(local_build, 'preflight'), \
                 patch.object(local_build, 'verify_extraction'), \
                 patch.object(local_build.subprocess, 'run', side_effect=run):
                local_build.build(output)
            self.assertEqual(commands[-1][1], 'twrp')
            self.assertNotIn('--experimental', commands[-1])
            self.assertFalse(Path(str(output)+'.tar').exists())
