import sys
from pathlib import Path
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from copy_tree import copy_tree

class InputIsolationTests(unittest.TestCase):
    def test_mutable_staging_cannot_modify_locked_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source, dest = root / 'source', root / 'stage'
            source.mkdir()
            (source / 'file').write_bytes(b'original')
            (source / 'link').symlink_to('file')
            copy_tree(source, dest)
            self.assertNotEqual((source / 'file').stat().st_ino, (dest / 'file').stat().st_ino)
            (dest / 'file').write_bytes(b'modified')
            self.assertEqual((source / 'file').read_bytes(), b'original')
            self.assertTrue((dest / 'link').is_symlink())
            with self.assertRaises(ValueError):
                copy_tree(source, dest)
            with self.assertRaises(ValueError):
                copy_tree(source, source / 'nested')
