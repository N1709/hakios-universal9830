import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('ipsw_fetch', Path(__file__).resolve().parents[3] / 'sysimage/fetch-from-ipsw.py')
ipsw = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ipsw)

class ByteRangeTests(unittest.TestCase):
    def response(self, status, content_range):
        class Response:
            closed = False
            def close(self):
                self.closed = True
        result = Response()
        result.status = status
        result.headers = {'Content-Range': content_range}
        return result

    def test_server_ignoring_range_is_refused_before_download(self):
        r = self.response(200, None)
        with patch.object(ipsw.urllib.request, 'urlopen', return_value=r):
            with self.assertRaisesRegex(ValueError, 'byte range'):
                ipsw.fetch(10, 20)
        self.assertTrue(r.closed)

    def test_wrong_range_or_archive_length_is_refused(self):
        for value in ('bytes 0-10/21', 'bytes 10-20/21'):
            r = self.response(206, value)
            with patch.object(ipsw.urllib.request, 'urlopen', return_value=r):
                with self.assertRaises(ValueError):
                    ipsw.fetch(10, 20)
            self.assertTrue(r.closed)

    def test_exact_range_is_accepted(self):
        r = self.response(206, f'bytes 10-20/{ipsw.SIZE}')
        with patch.object(ipsw.urllib.request, 'urlopen', return_value=r):
            self.assertIs(ipsw.fetch(10, 20), r)
        self.assertFalse(r.closed)
