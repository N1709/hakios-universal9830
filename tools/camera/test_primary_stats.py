"""Compare the actual pinned and optimized ISP statistics on synthetic RAW10."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('profiles',Path(__file__).with_name('prepare-profile-sources.py'))
profiles=importlib.util.module_from_spec(spec)
spec.loader.exec_module(profiles)

class PrimaryStatsTests(unittest.TestCase):
    def test_primary_sampling_and_front_uw_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            out=Path(tmp)
            for name in ('camera_isp.h','redwood_camera.h'):
                (out/name).write_text(profiles.original(name))
            results={}
            for label,source in [('baseline',profiles.original('camera_isp.c')),('sparse',profiles.primary_stats_source())]:
                src=out/(label+'.c');src.write_text(source);exe=out/label
                subprocess.run(['clang','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-I'+str(out),str(ROOT/'tools/camera/test_primary_stats.c'),str(src),'-lm','-o',str(exe)],check=True)
                text=subprocess.check_output([str(exe)],text=True)
                results[label]=[list(map(float,line.split()))for line in text.splitlines()]
            for old,new in zip(results['baseline'],results['sparse']):
                if old[0]!=1:
                    self.assertEqual(old[1:5],new[1:5])
                else:
                    self.assertLess(new[1],old[1]*.26)
                    for before,after in zip(old[2:5],new[2:5]):
                        self.assertLess(abs(before-after),.01*abs(before))

if __name__=='__main__':unittest.main()
