"""Compile actual camera pool get/put with narrow IOKit allocation mocks."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
class PoolTests(unittest.TestCase):
 def test_allocations_and_failures(self):
  source=(ROOT/'xnu/files/iokit/Kernel/MiniIOSurface.cpp').read_text()
  globals=source[source.index('static int gCameraPoolSlot ='):source.index('extern "C" bool redwood_camera_dma_get')]
  code=source[source.index('extern "C" bool\nredwood_camera_dma_get'):source.index('\nvoid\nMiniIOSurfaceClearKept')]
  with tempfile.TemporaryDirectory() as tmp:
   path=Path(tmp);(path/'actual-pool.h').write_text(globals+code)
   subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined','-I'+tmp,str(ROOT/'tools/camera/test_dma_pool.cpp'),'-o',str(path/'test')],check=True)
   subprocess.run([str(path/'test')],check=True)
if __name__=='__main__':unittest.main()
