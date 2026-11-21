import hashlib
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from stock_return import inspect, plan

class StockReturnTests(unittest.TestCase):
    def archive(self, root, role, members, revision='G980FXXSNHYB7'):
        name = f'{role}_{revision}_test.tar.md5'
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode='w') as t:
            for member in members:
                info=tarfile.TarInfo(member);info.size=4;t.addfile(info,io.BytesIO(b'test'))
        data=buf.getvalue()
        p=root/name;p.write_bytes(data+hashlib.md5(data).hexdigest().encode())
        return p
    def firmware(self, root):
        self.archive(root,'BL',['sboot.bin.lz4'])
        self.archive(root,'AP',['boot.img.lz4','recovery.img.lz4','super.img.lz4','vbmeta.img.lz4'])
        self.archive(root,'CP',['modem.bin.lz4'])
        self.archive(root,'CSC',['cache.img.lz4','X1S.pit'])
    def test_full_stock_plan_is_read_only_and_reports_data_reset(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);self.firmware(root);p=plan(root)
            self.assertFalse(p['executable']);self.assertFalse(p['data_preservation'])
            self.assertEqual(p['files']['CSC']['embedded_pit'],['X1S.pit'])
            self.assertTrue(all(x['odin_md5_checked'] for x in p['files'].values()))
    def test_corrupt_archive_and_boot_only_ap_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);p=self.archive(root,'AP',['boot.img.lz4'])
            with self.assertRaisesRegex(ValueError,'full stock'):inspect(p,'AP')
            data=bytearray(p.read_bytes());data[100]^=1;p.write_bytes(data)
            with self.assertRaisesRegex(ValueError,'MD5'):inspect(p,'AP')
    def test_home_csc_and_wrong_revision_are_not_return_to_stock_plans(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);self.firmware(root)
            csc=next(root.glob('CSC_*'));csc.rename(root/('HOME_'+csc.name))
            with self.assertRaisesRegex(ValueError,'CSC'):plan(root)
            (root/('HOME_'+csc.name)).rename(csc)
            old=next(root.glob('BL_*'));old.rename(root/old.name.replace('G980FXXSNHYB7','G980FXXS1AAAA'))
            with self.assertRaisesRegex(ValueError,'revision'):plan(root)

    def test_parent_paths_and_symlink_inputs_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);p=self.archive(root,'CSC',['../efs'])
            with self.assertRaisesRegex(ValueError,'unsafe'):inspect(p,'CSC')
            link=root/'alias.tar.md5';link.symlink_to(p)
            with self.assertRaisesRegex(ValueError,'regular'):inspect(link,'CSC')
