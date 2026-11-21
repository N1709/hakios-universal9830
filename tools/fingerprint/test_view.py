#!/usr/bin/env python3
"""Synthetic message-buffer snapshots; importing the view must not contact a phone."""
import runpy
from pathlib import Path
import struct
import unittest
view = runpy.run_path(str(Path(__file__).with_name('fp-anim.py')))
LogCursor = view['LogCursor']


def snap(total, text): return struct.pack('<Q', total) + text


class CursorTests(unittest.TestCase):
    def test_history_duplicate_and_partial(self):
        c = LogCursor()
        self.assertEqual(c.feed(snap(10, b'old MATCH\n')), [])
        self.assertEqual(c.feed(snap(10, b'old MATCH\n')), [])
        self.assertEqual(c.feed(snap(14, b'prefixnew\n')), ['new'])
        self.assertEqual(c.feed(snap(17, b'prefixpar')), [])
        self.assertEqual(c.feed(snap(22, b'prefixtial\n')), ['partial'])

    def test_reboot_and_overwrite(self):
        c = LogCursor(); c.feed(snap(100, b'old\n'))
        self.assertEqual(c.feed(snap(2, b'\0\0')), [])
        self.assertTrue(c.rebooted)
        self.assertEqual(c.feed(snap(50, b'partial\nfresh\n')), ['fresh'])
        self.assertFalse(c.rebooted)
        with self.assertRaises(ValueError): c.feed(b'bad')

    def test_candidate_is_distinct_from_nomatch(self):
        line = view['LINE'].search('socket noise redwood-tee: fp run 6: NO MATCH')
        self.assertEqual(line.group(1), 'fp run 6: NO MATCH')
        event = view['event']; scope = event.__globals__
        scope['state'], scope['progress'] = 'done', 100
        event('fp run 6 step 1: op 1 -> result 0 status 0x6 op 4 error 0 timeout -1')
        self.assertEqual(scope['mode'], 6)
        self.assertEqual(scope['progress'], 0)
        self.assertEqual(scope['state'], 'white')
        event('fp run 6: NO MATCH')
        self.assertEqual(scope['state'], 'nomatch')
        event('fp run 6: SENSOR MATCH candidate (bio 1 score 0; token not verified)')
        self.assertEqual(scope['state'], 'match')
        event('fp run 6: invalid response: stop')
        self.assertEqual(scope['state'], 'grey')


if __name__ == '__main__': unittest.main()
