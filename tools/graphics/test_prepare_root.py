import subprocess
import tempfile
import unittest
from pathlib import Path
REPO=Path(__file__).resolve().parents[2]
class GraphicsRootTests(unittest.TestCase):
 def test_shared_graphics_preserves_auth_and_is_idempotent(self):
  with tempfile.TemporaryDirectory() as tmp:
   root=Path(tmp)/'root';bins=Path(tmp)/'bin';(root/'etc').mkdir(parents=True);bins.mkdir()
   rc='# boot\n/redwood/bin/metal_props on\nexec /redwood/bin/softkeybag-init /sbin/launchd.ios\n'
   (root/'etc/rc').write_text(rc)
   rule='/redwood/lib/libredwood-media.dylib mediaserverd'
   (root/'etc/inject.conf').write_text(rule+'\n/var/tmp/maps-layer-rasterize.dylib Maps\n')
   for name in ['RedwoodMetal','libredwoodair.dylib','libredwood-graphics.dylib','metal_props']:(bins/name).write_bytes(name.encode())
   command=['python3',str(REPO/'tools/graphics/prepare-root.py'),str(root),'--binaries',str(bins)]
   subprocess.run(command,check=True,stdout=subprocess.DEVNULL)
   before={p.relative_to(root):p.read_bytes() for p in root.rglob('*') if p.is_file()}
   subprocess.run(command,check=True,stdout=subprocess.DEVNULL)
   after={p.relative_to(root):p.read_bytes() for p in root.rglob('*') if p.is_file()}
   self.assertEqual(before,after)
   text=(root/'etc/rc').read_text();self.assertEqual(text.count('/redwood/bin/metal_props auto'),1);self.assertIn(rc.splitlines()[2],text)
   conf=(root/'etc/inject.conf').read_text();self.assertIn(rule,conf);self.assertNotIn('/var/tmp/',conf);self.assertNotIn('libredwood-metaloff',conf);self.assertNotIn('metal_props on',text)
   shared=next(line for line in conf.splitlines() if 'libredwood-graphics' in line)
   self.assertIn('/var/containers/Bundle/Application/',shared);self.assertNotIn('Maps',shared);self.assertNotIn('FindMy',shared)
   self.assertEqual((root/'lib/RedwoodMetal.bundle/libredwoodair.dylib').read_bytes(),b'libredwoodair.dylib')
if __name__=='__main__':unittest.main()
