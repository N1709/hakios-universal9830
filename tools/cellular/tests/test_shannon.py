"""Synthetic malformed-input tests; no firmware, SIM data or phone needed."""
import importlib.util
import io
import re
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
from shannon import ENTRY, read_toc, sipc5_header

spec = importlib.util.spec_from_file_location('memory_audit', TOOLS / 'memory-audit.py')
memory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(memory)


def image(records=None):
    boot, main = b'boot-test', b'main-test'
    if records is None:
        records = [
            ('TOC', 0, 0x40008000, 512, 0, 7),
            ('BOOT', 512, 0x40000000, len(boot), zlib.crc32(boot), 1),
            ('MAIN', 512 + len(boot), 0x40010000, len(main), zlib.crc32(main), 2),
            ('NV', 0, 0x4b500000, 1048576, 0, 5),
        ]
    header = b''.join(ENTRY.pack(n.encode(), *values) for n, *values in records)
    return header.ljust(512, b'\0') + boot + main


class TocTests(unittest.TestCase):
    def parse(self, data, verify=True):
        return read_toc(io.BytesIO(data), len(data), verify)

    def test_good_crc_and_external_nv(self):
        entries = self.parse(image())
        self.assertTrue(entries[1]['crc_verified'])
        self.assertIn('external EFS', entries[-1]['source'])
        self.assertNotIn('crc_verified', entries[-1])

    def test_crc_corruption(self):
        data = bytearray(image())
        data[512] ^= 1
        with self.assertRaisesRegex(ValueError, 'BOOT CRC'):
            self.parse(data)

    def test_short_table_and_payload(self):
        for data in (b'TOC', image()[:-1]):
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                self.parse(data)

    def test_duplicate_unknown_missing_magic(self):
        for name in (b'TOC', b'WHAT', b''):
            data = bytearray(image())
            data[32:44] = name.ljust(12, b'\0')
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.parse(data, False)
        data = bytearray(image())
        data[:12] = b'BOOT'.ljust(12, b'\0')
        with self.assertRaisesRegex(ValueError, 'magic'):
            self.parse(data)

    def test_sections_outside_file_or_overlapping(self):
        for start, size in ((511, 9), (0xffffffff, 9), (512, 0xffffffff), (512, 0)):
            data = bytearray(image())
            struct.pack_into('<I', data, 64 + 12, start)
            struct.pack_into('<I', data, 64 + 20, size)
            with self.subTest(start=start, size=size), self.assertRaises(ValueError):
                self.parse(data, False)

    def test_cp_address_overflow(self):
        data = bytearray(image())
        struct.pack_into('<I', data, 32 + 16, 0xffffffff)
        with self.assertRaisesRegex(ValueError, 'overflows'):
            self.parse(data)

    def test_boot_spi_limit(self):
        data = bytearray(image().ljust(200000, b'\0'))
        struct.pack_into('<I', data, 32 + 20, 128 * 1024 + 1)
        with self.assertRaisesRegex(ValueError, 'SPI loader limit'):
            self.parse(data, False)

    def test_toc_certificate_overlap(self):
        data = bytearray(image())
        struct.pack_into('<I', data, 20, 520)
        with self.assertRaisesRegex(ValueError, 'signature span'):
            self.parse(data, False)


class SipcTests(unittest.TestCase):
    def test_normal_and_unaligned_input(self):
        data = memoryview(b'x\xf8\xeb\x06\x00ab')[1:]
        self.assertEqual(sipc5_header(data)['payload_size'], 2)

    def test_extended_length(self):
        data = b'\xfa\x0a' + struct.pack('<I', 65536) + bytes(65530)
        self.assertEqual(sipc5_header(data)['frame_size'], 65536)

    def test_control_is_not_extended_length(self):
        result = sipc5_header(b'\xfb\xeb\x06\x00\xffa')
        self.assertEqual(result['header_size'], 5)
        self.assertEqual(result['control'], 255)
        self.assertEqual(result['payload_size'], 1)

    def test_arm64_padding(self):
        data = b'\xfc\x0a\x09\x00abcde' + bytes(7)
        self.assertEqual(sipc5_header(data)['total_size'], 16)
        self.assertEqual(sipc5_header(data, alignment=4)['total_size'], 12)
        with self.assertRaisesRegex(ValueError, 'padding'):
            sipc5_header(data[:12])

    def test_bad_header_lengths_and_resource_limit(self):
        cases = (b'', b'\x00abc', b'\xf8\xeb\x03\x00', b'\xfa\xeb\x04\x00',
                 b'\xf9\xeb\x04\x00', b'\xf8\xeb\x06\x00a',
                 b'\xfa\xeb\xff\xff\xff\xff')
        for data in cases:
            with self.subTest(data=data), self.assertRaises(ValueError):
                sipc5_header(data)
        with self.assertRaises(ValueError):
            sipc5_header(b'\xf8\xeb\x04\x00', alignment=16)

    def test_all_short_prefixes(self):
        frame = b'\xff\xeb\x09\x00\x01abcd' + bytes(7)
        self.assertEqual(sipc5_header(frame)['total_size'], len(frame))
        for length in range(len(frame)):
            with self.subTest(length=length), self.assertRaises(ValueError):
                sipc5_header(frame[:length])


class MemoryTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.legacy = Path(self.directory.name)
        (self.legacy / 'xnuloader').mkdir()
        (self.legacy / 'xnuloader/loader.c').write_text(
            '#define DOWNLOAD_BASE 0xF0000000ULL\n#define DOWNLOAD_MAX_SIZE 0x01000000ULL\n')
        sound_path = Path('xnu/files/bsd/dev/redwood_exynos_sound.c')
        (self.legacy / sound_path).parent.mkdir(parents=True)
        (self.legacy / sound_path).write_text((TOOLS.parents[1] / sound_path).read_text())

    def test_half_open_ranges(self):
        self.assertFalse(memory.overlaps(('a', 0x1000, 0x1000), ('b', 0x2000, 1)))
        self.assertTrue(memory.overlaps(('a', 0x1000, 0x1001), ('b', 0x2000, 1)))

    def test_uefi_cp_reservations_and_no_dram_overlap(self):
        source = (TOOLS.parents[1] / 'uefi-y2s/Platforms/Samsung/x1sltePkg/Library/'
                  'MemoryMapLib/MemoryMapLib.c').read_text()
        records = re.findall(r'\{"([^"]+)",\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),([^}]+)\}', source)
        by_name = {name: (int(base, 16), int(size, 16), attributes)
                   for name, base, size, attributes in records}
        for name, expected in zip(('CP IPC', 'CP MSI', 'CP Packet Memory'), memory.CP):
            base, size, attributes = by_name[name]
            self.assertEqual((base, size), expected[1:])
            self.assertIn('Reserv, UNCACHED_UNBUFFERED', attributes)
        self.assertNotIn('XNU Download', by_name)
        dram = sorted((base, base + size, name) for name, (base, size, _) in by_name.items()
                      if base >= 0x80000000)
        for previous, current in zip(dram, dram[1:]):
            self.assertLessEqual(previous[1], current[0], (previous[2], current[2]))

    def test_actual_source_conflicts_and_missing_evidence(self):
        report = memory.audit(self.legacy, '')
        self.assertIn(('cp_rmem', 'loader DOWNLOAD'),
                      [(c['cp'], c['owner']) for c in report['conflicts']])
        self.assertEqual(set(report['unknown']), {'display pool', 'wifi dma', 'XNU window',
                                                 'deployed UEFI CP reservations', 'deployed UEFI CP cache policy'})
        # Avoid a wrong claim that SLOG covers MSI: f2200000 + 2800000 = f4a00000.
        self.assertFalse(any(c['cp'] == 'cp_msi_rmem' for c in report['conflicts']))

    def test_saved_log_display_overlap(self):
        report = memory.audit(self.legacy, 'display pool: d3ff0000 size c000000\n'
                              'wifi dma: e8630000 size 2000000\n'
                              'phys window 880000000 size 17e000000, claim status 0\n')
        self.assertEqual(report['unknown'], ['deployed UEFI CP reservations', 'deployed UEFI CP cache policy'])
        self.assertIn(dict(cp='cp_rmem_2', owner='display pool',
                           overlap_start='0xd3ff0000', overlap_size='0x10000'), report['conflicts'])

    def test_dynamic_staging_requires_its_own_boot_evidence(self):
        report = memory.audit(TOOLS.parents[1], '')
        self.assertIn('loader DOWNLOAD', report['unknown'])
        self.assertFalse(report['conflicts'])
        log = ('download buffer: e0000000 size 1000000\n'
               'display pool: d4000000 size c000000\n'
               'wifi dma: e1000000 size 2000000\n'
               'phys window 880000000 size 17e000000, claim status 0\n'
               'cellular memory: reserved 3/3\ncellular memory: normal-nc 3/3\n')
        report = memory.audit(TOOLS.parents[1], log)
        self.assertFalse(report['unknown'])
        self.assertFalse(report['conflicts'])
        # A subsequent bad reservation report must win over an earlier good one.
        report = memory.audit(TOOLS.parents[1], log + 'cellular memory: reserved 2/3\n')
        self.assertIn('deployed UEFI CP reservations', report['unknown'])
        report = memory.audit(TOOLS.parents[1], log + 'cellular memory: normal-nc 0/3\n')
        self.assertIn('deployed UEFI CP cache policy', report['unknown'])
        report = memory.audit(TOOLS.parents[1], '== XNULOADER run (menu) ==\n' + log +
                              '== XNULOADER run (menu) ==\ncellular memory: reserved 3/3\n')
        self.assertIn('loader DOWNLOAD', report['unknown'])
        self.assertIn('display pool', report['unknown'])


if __name__ == '__main__':
    unittest.main()
