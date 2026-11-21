import importlib.util
from pathlib import Path
import tempfile
import unittest

spec=importlib.util.spec_from_file_location('profile_sources',Path(__file__).with_name('prepare-profile-sources.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class ProfileSourceTests(unittest.TestCase):
    def test_shared_declaration_ignores_comments(self):
        self.assertEqual(m.declaration('struct x { int a; /* yes */ };','x'),m.declaration('struct x{int a;};','x'))
        self.assertNotEqual(m.declaration('struct x { int a; };','x'),m.declaration('struct x{long a;};','x'))
    def test_pinned_original_isp_and_front_default(self):
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp); report=m.prepare(out)
            for name in ('camera_isp.c','camera_isp.h','redwood_camera.h'):
                self.assertEqual((out/'original'/name).read_text(),m.primary_stats_source() if name=='camera_isp.c' else m.original(name))
            source=(out/'original/camera_device.c').read_text()
            self.assertIn('if (!fp) return false;',source)
            self.assertIn('if(ultra)redwood_camera_experimental_worker(context);else worker_original_body(context);',source)
            self.assertIn('if(s->camera->sensor!=1)return front_experimental_profile()?',source)
            self.assertEqual(report['default_front'],'original')
            self.assertIn('s20_zoom_clamp((float)zoom,s->camera->sensor==1',source)
            self.assertIn('redwood-ultrawide-enable',source)
            self.assertIn('(head.sensor==3||head.sensor==4)',source)
            self.assertIn('s20_sensor_is_rear(head.sensor)',(out/'experimental-device.c').read_text())
            self.assertIn('!s20_sensor_is_rear(f->sensor)',(out/'experimental-device.c').read_text())
            self.assertNotIn('f->width != 2016 && f->width != 4032',(out/'experimental-device.c').read_text())
            self.assertIn('redwood-primary-legacy-black',source)
            self.assertIn('fw->gain_unity > 0 && primary_legacy_black',source)
            self.assertIn('redwood-primary-contrast',source)
            self.assertIn('redwood-primary-full',source)
            self.assertIn('fb->width == 4032 && primary_legacy_black',source)
            self.assertIn('fd->gain_unity > 0 && primary_legacy_black',source)
            self.assertIn('isp.zoom=s->camera->sensor==1?rear_requested_zoom(s):s->zoom;',source)
            # Old front's globals are confined to its original object; new ISP APIs use separate symbols.
            self.assertIn('extern int isp_black4[4]',(out/'original/camera_isp.h').read_text())
            self.assertIn('#define isp_measure experimental_isp_measure',(out/'experimental-symbols.h').read_text())

if __name__=='__main__':unittest.main()
