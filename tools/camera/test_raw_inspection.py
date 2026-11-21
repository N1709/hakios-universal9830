"""The RAW diagnostic must reject partial transfers and preserve all CFA/low bits."""
import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('raw_inspection', Path(__file__).with_name('inspect-frame-raw.py'))
raw_inspection = importlib.util.module_from_spec(spec)
spec.loader.exec_module(raw_inspection)


class RawInspectionTests(unittest.TestCase):
    def frame(self):
        data = bytearray(120 + 20 * 16)
        struct.pack_into('<8s7I', data, 0, b'RWFRAME1', 120, 16, 16, 20, 1, 1, 64)
        struct.pack_into('<12I', data, 56, 1000, 1468, 128, 32, 512, 22564, 22564, 65535, 40, 1, 0, 0)
        for y in range(16):
            for x in range(0, 16, 4):
                at = 120 + y*20 + x//4*5
                for dx in range(4):
                    v = (61, 62, 63, 64)[(y&1)*2+(dx&1)]
                    data[at+dx] = v >> 2
                    data[at+4] |= (v&3) << (2*dx)
        return data

    def test_sites_and_metadata(self):
        out = raw_inspection.inspect(self.frame(), 2, 2)
        self.assertEqual(out['sensor'], 1)
        self.assertEqual(out['gain'], 128)
        for row in out['tiles']:
            for tile in row:
                self.assertEqual([v['mean'] for v in tile], [61, 62, 63, 64])
                self.assertTrue(all(v['std'] == 0 for v in tile))

    def test_partial_and_extra_rejected(self):
        for data in (self.frame()[:-1], self.frame()+b'x', b'RWFRAME1'):
            with self.assertRaises(ValueError):
                raw_inspection.inspect(data, 2, 2)

    def test_invalid_stride_rejected(self):
        data = self.frame()
        struct.pack_into('<I', data, 20, 1)
        with self.assertRaises(ValueError):
            raw_inspection.inspect(data, 2, 2)


if __name__ == '__main__':
    unittest.main()
