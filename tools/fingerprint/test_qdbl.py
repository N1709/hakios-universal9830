#!/usr/bin/env python3
import struct
import unittest
from qdbl import summarize


def record(kind, size):
    b = bytearray(size)
    struct.pack_into('<BBH', b, 0, 0x28, kind, size)
    return b


class DiagnosticTests(unittest.TestCase):
    def test_selected_fields_only(self):
        verify = record(7, 69); verify[0x12:0x14] = b'\x81\xff'
        struct.pack_into('<HH', verify, 0x14, 1560, 2300)
        outcome = record(11, 24); outcome[0x12] = 2
        result = summarize(verify + outcome)
        self.assertEqual(result['record_counts'], {7: 1, 11: 1})
        self.assertEqual(result['verify_metrics'], [dict(metric_b3=8, metric_b4=1, metric_b5=255,
                                                               metric_b6=1560, metric_b7=2300)])
        self.assertEqual(result['identify_outcomes'], [dict(matched_flag=0, metric_cb=2, aed=0)])

    def test_bad_frames(self):
        for blob in (b'bad', record(7, 69)[:-1], record(7, 68), record(99, 14), b'\0' * 0x8001):
            with self.assertRaises(ValueError): summarize(blob)
        self.assertEqual(summarize(b'')['record_counts'], {})


if __name__ == '__main__': unittest.main()
