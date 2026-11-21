#!/usr/bin/env python3
"""Offline container boundary checks using synthetic bytes only."""
import importlib.util
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location('setfile', Path(__file__).with_name('inspect-samsung-setfile.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def fixture():
    return (struct.pack('<II4s4sIII', module.MAGIC, 1 << 3, b'TEST', b'0001', 2, 1, 48)
            + struct.pack('<5I', 0, 0xffffffff, 1, 0, 4) + b'data')


class ContainerTests(unittest.TestCase):
    def test_valid(self):
        result = module.inspect(fixture())
        self.assertEqual(result['groups'][0]['design'], '3AA_ISP')
        self.assertEqual(result['groups'][0]['entries'][0]['scenarios'], [0])
        self.assertEqual(result['groups'][0]['entries'][0]['offset'], 48)

    def test_truncated(self):
        for size in (0, 3, 16, 27, 30, 40, 47, 51):
            with self.subTest(size=size), self.assertRaises(ValueError):
                module.inspect(fixture()[:size])

    def test_invalid_regions_and_indexes(self):
        for offset, value in ((0, 0), (4, 0), (16, 0xffffffff), (24, 0),
                              (28, 2), (36, 0xffffffff), (40, 0xffffffff), (44, 0xffffffff)):
            data = bytearray(fixture())
            struct.pack_into('<I', data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                module.inspect(data)

    def test_v2_huge_ip_count(self):
        data = struct.pack('<4I', module.MAGIC - 1, 1, 0xffffffff, 16)
        with self.assertRaises(ValueError):
            module.inspect(data)

    def test_v2(self):
        data = struct.pack('<9I', module.MAGIC - 1, 1, 1, 32, 0, 1, 0, 4, 123)
        self.assertEqual(module.inspect(data)['format'], 2)


if __name__ == '__main__':
    unittest.main()
