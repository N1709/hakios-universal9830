import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from extraction import OUTPUTS, RECIPES, write, verify

class ExtractionTests(unittest.TestCase):
    def test_receipt_requires_matching_recipe_and_actual_contents(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in RECIPES:
                p = root / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text('recipe')
            for name in OUTPUTS:
                p = root / 'sysimage' / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text('content')
            with self.assertRaisesRegex(ValueError, 'missing extraction'):
                verify(root)
            write(root)
            verify(root)
            (root / 'sysimage/rootfs').write_text('changed')
            with self.assertRaisesRegex(ValueError, 'changed'):
                verify(root)
            write(root)
            (root / RECIPES[0]).write_text('new recipe')
            with self.assertRaisesRegex(ValueError, 'changed'):
                verify(root)
