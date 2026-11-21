"""Exercise clean-root Camera assembly with no pre-existing data files."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import plistlib

spec=importlib.util.spec_from_file_location('camera_root',Path(__file__).with_name('prepare-root.py'))
module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
class ImageTests(unittest.TestCase):
    def test_clean_seed_and_idempotence(self):
        with tempfile.TemporaryDirectory() as tmp:
            base=Path(tmp);root=base/'root';binaries=base/'bin';binaries.mkdir()
            (root/'etc').mkdir(parents=True);(root/'lib').mkdir()
            (root/'lib/libredwood-tweaks.dylib').write_bytes(b'loader')
            (root/'etc/inject.conf').write_text('/redwood/lib/libredwood-tweaks.dylib Preferences imagent\nother SpringBoard\n')
            (root/'etc/seed.list').write_text('755\t0\t0\t/private/var/phone\t/seed/phone\n')
            for name in ['libredwood-media.dylib',*(n+'.dylib' for n in module.HOOKS)]:
                (binaries/name).write_bytes(name.encode())
            module.prepare(root,binaries)
            first={str(p.relative_to(root)):p.read_bytes() for p in root.rglob('*') if p.is_file()}
            module.prepare(root,binaries)
            self.assertEqual(first,{str(p.relative_to(root)):p.read_bytes() for p in root.rglob('*') if p.is_file()})
            self.assertIn('Preferences imagent Camera',(root/'etc/inject.conf').read_text())
            self.assertIn('d\t755\t501\t501\t'+module.CACHE,(root/'etc/var.skel').read_text())
            self.assertIn('other SpringBoard',(root/'etc/inject.conf').read_text())
            self.assertIn('/private/var/phone',(root/'etc/seed.list').read_text())
            # Init's copy-if-absent manifest can populate an empty data volume.
            data=base/'data'
            for line in (root/'etc/seed.list').read_text().splitlines()[1:]:
                mode,uid,gid,destination,source=line.split('\t')
                target=data/destination.lstrip('/');target.parent.mkdir(parents=True,exist_ok=True)
                target.write_bytes((root/source.lstrip('/')).read_bytes())
            tweak=data/'private/var/jb/usr/lib/TweakInject/RedwoodCameraLenses.plist'
            self.assertEqual(plistlib.loads(tweak.read_bytes())['Filter']['Bundles'],['com.apple.camera'])
            self.assertTrue((data/(module.CACHE.lstrip('/')+'/redwood-ultrawide-enable')).is_file())
            self.assertTrue((data/(module.CACHE.lstrip('/')+'/redwood-gw2-enable')).is_file())
            self.assertLess(len((root/'etc/seed.list').read_bytes()),4096)
    def test_missing_artifacts_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(SystemExit):module.prepare(Path(tmp),Path(tmp))
if __name__=='__main__':unittest.main()
