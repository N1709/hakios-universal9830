"""Compile/test the actual generated primary AF controller without hardware."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('profiles',Path(__file__).with_name('prepare-profile-sources.py'))
profiles=importlib.util.module_from_spec(spec);spec.loader.exec_module(profiles)
class AFTests(unittest.TestCase):
    def test_controller(self):
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp)
            source=profiles.primary_af_source(profiles.original('camera_device.c'))
            a=source.index('enum { AF_WATCH');b=source.index("/* the sensor's colour tuning",a)
            (out/'actual-af.h').write_text(source[a:b])
            for name in ('camera_isp.h','redwood_camera.h'):(out/name).write_text(profiles.original(name))
            exe=out/'af-test'
            subprocess.run(['clang','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                '-I'+str(out),str(ROOT/'tools/camera/test_primary_af.c'),'-lm','-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)
if __name__=='__main__':unittest.main()
