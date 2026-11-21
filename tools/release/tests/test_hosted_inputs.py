import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from cache import digest, fingerprint
import hosted_inputs as hosted


class HostedInputsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.source = self.base / 'source'
        self.source.mkdir()
        for name in hosted.REQUIRED:
            p = self.source / name
            if name in ('inputs/toolchain.json', 'userland/hfs-kit.bin'):
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b'reviewed fixture')
            else:
                p.mkdir(parents=True, exist_ok=True)
                (p / 'fixture').write_bytes(b'reviewed fixture')
        jb = self.source / 'jb/root/usr/bin'
        jb.mkdir(parents=True)
        (jb / 'tool').write_bytes(b'program')
        (jb / 'runtime-link').symlink_to('/var/jb/usr/bin/tool')
        names = hosted.REQUIRED | {'jb'}
        records = {n: fingerprint({n: self.source / n}, {}) for n in names}
        (self.source / 'inputs/private-inputs.lock.json').write_text(json.dumps({'inputs': records}))
        self.archive = self.base / 'board-inputs.tar.gz'
        self.dest = self.base / 'dest'
        self.dest.mkdir()

    def test_roundtrip_preserves_bootstrap_links_and_modes(self):
        hosted.pack(self.source, self.archive)
        hosted.unpack(self.archive, self.dest, digest(self.archive))
        self.assertEqual((self.dest / 'jb/root/usr/bin/runtime-link').readlink(), Path('/var/jb/usr/bin/tool'))
        for name in hosted.REQUIRED | {'jb'}:
            self.assertEqual(fingerprint({name: self.source / name}, {}), fingerprint({name: self.dest / name}, {}))
        self.assertFalse((self.dest / 'sysimage').exists())
        self.assertFalse((self.dest / 'inputs/private-inputs.lock.json').exists())

    def test_modified_reviewed_inputs_refused(self):
        (self.source / 'fw-fingerprint/fixture').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'hash mismatch'):
            hosted.pack(self.source, self.archive)

    def test_wifi_credentials_refused(self):
        (self.source / 'fw-wifi/network').write_text('personal')
        with self.assertRaisesRegex(ValueError, 'Wi-Fi'):
            hosted.pack(self.source, self.archive)

    def test_bad_transport_hash_never_extracts(self):
        hosted.pack(self.source, self.archive)
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            hosted.unpack(self.archive, self.dest, '0' * 64)
        self.assertEqual(list(self.dest.iterdir()), [])

    def altered_archive(self, name, symlink=False):
        hosted.pack(self.source, self.archive)
        out = self.base / 'altered.tar.gz'
        with tarfile.open(self.archive) as src, tarfile.open(out, 'w:gz') as dst:
            for m in src.getmembers():
                dst.addfile(m, src.extractfile(m) if m.isfile() else None)
            info = tarfile.TarInfo(name)
            if symlink:
                info.type, info.linkname = tarfile.SYMTYPE, '/tmp'
                dst.addfile(info)
            else:
                info.size = 4
                dst.addfile(info, io.BytesIO(b'evil'))
        return out

    def test_path_escape_refused_before_any_writes(self):
        archive = self.altered_archive('../escape')
        with self.assertRaisesRegex(ValueError, 'unsafe'):
            hosted.unpack(archive, self.dest, digest(archive))
        self.assertEqual(list(self.dest.iterdir()), [])

    def test_write_through_runtime_link_refused(self):
        archive = self.altered_archive('jb/root/usr/bin/runtime-link/escape')
        with self.assertRaisesRegex(ValueError, 'through a symlink'):
            hosted.unpack(archive, self.dest, digest(archive))
        self.assertEqual(list(self.dest.iterdir()), [])

    def test_existing_destination_symlink_refused(self):
        hosted.pack(self.source, self.archive)
        (self.dest / 'fw-wifi').symlink_to(self.base, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'destination contains a symlink'):
            hosted.unpack(self.archive, self.dest, digest(self.archive))


if __name__ == '__main__':
    unittest.main()
