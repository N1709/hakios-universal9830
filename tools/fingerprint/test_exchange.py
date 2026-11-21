#!/usr/bin/env python3
"""Synthetic protocol and local-file tests; no device, credentials or real biometrics."""
import os
from contextlib import nullcontext
from pathlib import Path
import stat
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parent))
import wire
import exchange


class WireTests(unittest.TestCase):
    def setUp(self):
        self.lock_patch = patch.object(exchange, 'session_lock', return_value=nullcontext())
        self.lock_patch.start()
        self.addCleanup(self.lock_patch.stop)

    def reply(self, command=1):
        packet = wire.request(command)
        wire.put32(packet, 0x177004, command)
        wire.put32(packet, 0x177008, 0)
        return wire.ENVELOPE.pack(b'FPR1', 0, 4, command, wire.SIZE) + packet

    def test_request_boundaries(self):
        self.assertEqual(wire.validate_request(wire.prepare(b'x' * 0xa000)), 1)
        for factory in (lambda: wire.prepare(b'x' * 0xa001),
                        lambda: wire.identify_init(b''),
                        lambda: wire.identify_init(b'x' * 0x64001),
                        lambda: wire.identify_init(b'x', b'x' * 0x401),
                        lambda: wire.control(66, b'x' * 0xfa001),
                        lambda: wire.control(66, output_word=0xfa001),
                        lambda: wire.request(8)):
            with self.assertRaises(ValueError): factory()
        p = wire.prepare()
        wire.put32(p, 4, 2)
        with self.assertRaises(ValueError): wire.validate_request(p)

    def test_control_uses_control_lengths(self):
        p = wire.control(66, b'calibration-fixture', b'8', output_word=8)
        self.assertEqual(wire.u32(p, 12), 66)
        self.assertEqual(wire.u32(p, 0xfa110), 19)
        self.assertEqual(wire.u32(p, 0xa010), 0)
        self.assertEqual(wire.u32(p, 0x271014), 8)
        for local in (0x16, 0x31, 0x5b, 0x5c, 0x6c):
            with self.assertRaises(ValueError): wire.control(local)
        p = wire.request(13)
        wire.put32(p, 0x451, 0x401)
        with self.assertRaises(ValueError): wire.validate_request(p)

    def test_path_binding(self):
        path = '/var/root/fp-research/user-0'
        p = wire.storepath(path)
        self.assertEqual(wire.u32(p, 12), 0x30)
        self.assertEqual(wire.u32(p, 0xfa110), 100)
        self.assertEqual(p[0x110:0x174], path.encode().ljust(100, b'\0'))
        legacy = wire.identify_init(b'encrypted-fixture')
        self.assertEqual(legacy[0x6455d:0x645c1], b'fingerprint'.ljust(100, b'\0'))
        p = wire.identify_init(b'encrypted-fixture', path=path)
        self.assertEqual(p[0x6455d:0x645c1], path.encode().ljust(100, b'\0'))
        for invalid in ('', 'x' * 100, 'x\0y'):
            with self.assertRaises(ValueError): wire.storepath(invalid)

    def test_reply_envelope_and_ta_errors(self):
        blob = self.reply()
        summary, _ = wire.decode_reply(blob, 1)
        self.assertEqual(summary['error'], 0)
        for bad in (blob[:-1], blob + b'x', b'FAIL' + blob[4:]):
            with self.assertRaises(ValueError): wire.decode_reply(bad, 1)
        with self.assertRaises(ValueError): wire.decode_reply(blob, 5)
        blob = bytearray(blob)
        wire.put32(blob, 20 + wire.ERROR, 31)
        with self.assertRaises(ValueError): wire.decode_reply(blob, 1)
        wire.put32(blob, 20 + wire.RESPONSE + 4, 31)
        self.assertEqual(wire.decode_reply(blob, 1)[0]['error'], 31)
        wire.put32(blob, 20 + 0x181014, 0xa001)
        with self.assertRaises(ValueError): wire.decode_reply(blob, 1)

    def test_wrong_kernel_aborts_before_upload(self):
        with tempfile.TemporaryDirectory() as d:
            directory = Path(d)
            directory.chmod(0o700)
            req, reply = directory/'request', directory/'reply'
            req.write_bytes(wire.prepare()); req.chmod(0o600)
            with patch.object(exchange, 'remote', return_value=b'fingerprint wire: audit-v1') as remote:
                with self.assertRaises(ValueError): exchange.exchange(req, reply)
                remote.assert_called_once_with(exchange.STATUS)
            self.assertFalse(reply.exists())

    def test_private_request_and_reply(self):
        with tempfile.TemporaryDirectory() as d:
            directory = Path(d); directory.chmod(0o700)
            req, reply = directory/'request', directory/'reply'
            req.write_bytes(wire.prepare()); req.chmod(0o644)
            with self.assertRaises(ValueError): exchange.private_input(req)
            req.chmod(0o600)
            link = directory/'linked'; os.link(req, link)
            with self.assertRaises(ValueError): exchange.private_input(req)
            link.unlink(); link.symlink_to(req)
            with self.assertRaises(ValueError): exchange.private_input(link)
            link.unlink()
            done = (exchange.MARKER + '\nlast: fp exchange: done rc 0 transport 0 origin 4 cmd 1').encode()
            with patch.object(exchange, 'remote', side_effect=[exchange.MARKER.encode(), b'', b'', done, self.reply()]):
                self.assertEqual(exchange.exchange(req, reply)['command'], 1)
            self.assertEqual(stat.S_IMODE(reply.stat().st_mode), 0o600)
            with patch.object(exchange, 'remote') as remote:
                with self.assertRaises(FileExistsError): exchange.exchange(req, reply)
                remote.assert_not_called()


class LockTests(unittest.TestCase):
    def test_concurrent_clients_are_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            directory = Path(d)
            with exchange.session_lock(directory):
                with self.assertRaises(BlockingIOError):
                    with exchange.session_lock(directory): pass
            with exchange.session_lock(directory): pass

    def test_symlink_lock_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            directory = Path(d)
            target = directory/'target'; target.write_bytes(b'')
            (directory/'exchange.lock').symlink_to(target)
            with self.assertRaises(OSError):
                with exchange.session_lock(directory): pass


if __name__ == '__main__': unittest.main()
