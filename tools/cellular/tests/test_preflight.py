import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location('preflight', Path(__file__).parents[1] / 'preflight-report.py')
preflight = importlib.util.module_from_spec(spec)
spec.loader.exec_module(preflight)


class ReportTests(unittest.TestCase):
    def setUp(self):
        self.manifest = dict(installed_firmware_preserved=True, installed_loader_fallback=True,
                             preflight_report_pa='0xe8118650', preflight_report_size=8192)

    def test_address_and_count_validation(self):
        for address in ('0x15c30000', '0xe817fff0', '0xe8118651'):
            with self.subTest(address=address), self.assertRaises(ValueError):
                preflight.report_region(dict(self.manifest, preflight_report_pa=address))
        for response in ('0xe8118654: 00000000', '0xe8118650: 00000000\nERROR',
                         '0xe8118650: 00000000 00000000'):
            with self.subTest(response=response), self.assertRaises(ValueError):
                preflight.decode_words(0xe8118650, 1, response)
        self.assertEqual(preflight.decode_words(0xe8118650, 1, '0xe8118650: 304d5043\n'), b'CPM0')

    def test_bounded_chunked_report_and_invalid_header(self):
        report = b'allocator evidence\n' * 80
        memory = struct.pack('<II', 0x304d5043, len(report)) + report + bytes(8192-len(report)-8)
        reads = []
        def read(address, count):
            self.assertLessEqual(count, 64)
            offset = address - 0xe8118650
            self.assertGreaterEqual(offset, 0)
            self.assertLessEqual(offset+count*4, 8192)
            reads.append((address,count))
            return memory[offset:offset+count*4]
        self.assertEqual(preflight.collect(self.manifest, read), report)
        self.assertGreater(len(reads), 2)
        for magic, length in ((0, 10), (0x304d5043, 0), (0x304d5043, 8185)):
            with self.subTest(length=length), self.assertRaises(ValueError):
                preflight.collect(self.manifest, lambda *_: struct.pack('<II', magic, length))


if __name__ == '__main__':
    unittest.main()
