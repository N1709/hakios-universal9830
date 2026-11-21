import importlib.util
from pathlib import Path
import unittest
import struct
import tempfile
import zlib

spec = importlib.util.spec_from_file_location('prepare_m0', Path(__file__).parents[1] / 'prepare-m0.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class FallbackTests(unittest.TestCase):
    def setUp(self):
        self.original = [(b'loader.efi'.ljust(16, b'\0'), 0, b'original loader'),
                         (b'cmdline'.ljust(16, b'\0'), 7, b'original arguments\0')]

    def test_preflight_retains_exact_installed_loader_and_arguments(self):
        loader = b'MZ CELLULAR MEMORY PREFLIGHT'
        changed = prepare.replace_loader(self.original, loader, True)
        self.assertEqual(changed[0][2], loader)
        self.assertEqual(changed[1], (b'fallback.efi'.ljust(16, b'\0'), 0, b'original loader'))
        self.assertEqual(changed[2], (b'probe.report'.ljust(16, b'\0'), 0, bytes(16384)))
        self.assertEqual(changed[3], self.original[1])

    def test_reject_missing_or_inappropriate_fallback(self):
        with self.assertRaises(ValueError):
            prepare.replace_loader(self.original, b'MZ CELLULAR MEMORY PREFLIGHT')
        with self.assertRaises(ValueError):
            prepare.replace_loader(self.original, b'MZ normal loader', True)

    def test_cp_probe_marker_requires_and_preserves_fallback(self):
        loader = b'MZ CELLULAR CP BOOT PREFLIGHT'
        with self.assertRaises(ValueError):
            prepare.replace_loader(self.original, loader)
        changed = prepare.replace_loader(self.original, loader, True)
        self.assertEqual(changed[1][2], self.original[0][2])
        self.assertEqual(changed[3], self.original[1])

    def test_reject_recursive_or_ambiguous_backup(self):
        for records in ([], self.original + self.original[:1],
                        self.original + [(b'fallback.efi'.ljust(16, b'\0'), 0, b'old')]):
            with self.subTest(records=records), self.assertRaises(ValueError):
                prepare.replace_loader(records, b'MZ CELLULAR MEMORY PREFLIGHT', True)

    def test_explicit_boot_extraction_crc_and_metadata(self):
        boot = b'synthetic boot!!'
        main = b'synthetic main!!'
        entry = struct.Struct('<12s5I')
        table = b''.join(entry.pack(name.ljust(12,b'\0'),start,address,len(data),crc,index)
                         for name,start,address,data,crc,index in (
                             (b'TOC',0,0,bytes(512),0,0),
                             (b'BOOT',512,0x40000000,boot,zlib.crc32(boot),1),
                             (b'MAIN',528,0x50000000,main,zlib.crc32(main),2),
                             (b'NV',0,0,bytes(64),0,3)))
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'synthetic-radio.img'
            path.write_bytes(table.ljust(512,b'\0')+boot+main)
            data,info,metadata=prepare.cp_boot_payload(path)
            self.assertEqual(data,boot)
            self.assertEqual(struct.unpack('<4sIIIII',info),(b'S512',1,len(boot),zlib.crc32(boot),0x40000000,1))
            self.assertEqual(metadata['size'],16)
            image=bytearray(path.read_bytes());image[512]^=1;path.write_bytes(image)
            with self.assertRaises(ValueError):
                prepare.cp_boot_payload(path)


class KernelSegmentTests(unittest.TestCase):
    def test_fixed_segment_only_and_layout_refusal(self):
        records = [(name.ljust(16,b'\0'),0,value) for name,value in (
            (b'kpart',b'recovery\0'),(b'koff',b'2000000\0'),
            (b'rpart',b'recovery\0'),(b'roff',b'3400000\0'))]
        kernel=struct.pack('<II',0xfeedfacf,0x0100000c)+bytes(4088)
        image=bytearray(0x3401000)
        image[0x2000000:0x2000008]=kernel[:8]
        image[0x3000000:0x3000004]=b'KEEP'
        image[0x3400000:0x3400004]=b'ROOT'
        image[prepare.SLOT:prepare.SLOT+4]=b'XNUP'
        prepare.replace_kernel(image,kernel,records)
        self.assertEqual(image[0x2000000:0x2001000],kernel)
        self.assertEqual(image[0x3000000:0x3000004],b'KEEP')
        self.assertEqual(image[0x3400000:0x3400004],b'ROOT')
        self.assertEqual(image[prepare.SLOT:prepare.SLOT+4],b'XNUP')
        for invalid in (b'bad',bytes(4096),kernel+bytes(0x1000000)):
            with self.assertRaises(ValueError): prepare.replace_kernel(image,invalid,records)
        with self.assertRaises(ValueError): prepare.replace_kernel(image,kernel,records[:-1])


if __name__ == '__main__':
    unittest.main()
