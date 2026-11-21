import os
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release_config  # noqa: E402
from manage import ROOT, profile  # noqa: E402


class ReleaseConfigTests(unittest.TestCase):
    def conf(self, text, *overrides):
        with tempfile.NamedTemporaryFile('w', suffix='.conf', delete=False) as f:
            f.write(text)
        try:
            return release_config.load(f.name, overrides)
        finally:
            os.unlink(f.name)

    def test_repository_config_plans_s20_zips_and_skips_unmeasured(self):
        c = release_config.load(None, ['TARGETS=x1slte y2s', 'WRITES=true'])
        todo, skipped = release_config.plan(c)
        self.assertEqual([(t, o) for t, o, _ in todo], [('x1slte', 'twrp'), ('x1slte', 'restore')])
        self.assertEqual([t for t, _ in skipped], ['y2s'])
        n = release_config.names(c, 'x1slte', '20260101')
        self.assertEqual(n['twrp'], 'Jesvi-iOS_0.1.3_20260101_x1slte.zip')
        self.assertEqual(n['restore'], 'Jesvi-iOS-Restore_0.1.3_20260101_x1slte.zip')

    def test_preflight_names_say_so(self):
        c = self.conf('VERSION=1.2\nWRITES=false\n')
        n = release_config.names(c, 'x1slte', '20260101')
        self.assertTrue(n['twrp'].endswith('_preflight.zip'))
        self.assertTrue(n['restore'].endswith('_preflight.zip'))

    def test_bad_settings_fail(self):
        for text in ('BOGUS=1\n', 'OUTPUTS=odin\n', 'MODE=wipe\n', 'WRITES=yes\n',
                     'OUT=/tmp\n', 'OUT=dist/../..\n', 'NAME=a/b\n', 'TARGETS=\n'):
            with self.assertRaises(ValueError, msg=text):
                self.conf(text)
        with self.assertRaises(ValueError):
            self.conf('', 'NOPE=1')
        with self.assertRaises(ValueError):
            release_config.plan(self.conf('TARGETS=../x1slte\n'))

    def test_environment_overrides_file(self):
        os.environ['JESVI_VERSION'] = '9.9'
        try:
            self.assertEqual(self.conf('VERSION=1.0\n')['VERSION'], '9.9')
        finally:
            del os.environ['JESVI_VERSION']

    def test_profile_carries_measured_android_layout_outside_image_binding(self):
        d = profile('x1slte')
        self.assertEqual(d['android_super']['groups'][0]['name'], 'group_basic')
        self.assertGreater(d['layout']['android_stash_offset'], d['layout']['super_log_offset'])
        self.assertNotIn('android', (ROOT / 'devices/exynos990/x1slte/device.json').read_text())


if __name__ == '__main__':
    unittest.main()
