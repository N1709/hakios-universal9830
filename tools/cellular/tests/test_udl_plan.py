import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from udl_plan import build_plan


def table():
    names = ['TOC', 'BOOT', 'MAIN', 'APM', 'VSS', 'NV', 'OFFSET', 'INFO']
    return [dict(name=n, size=2049 if n == 'MAIN' else 32, entry_id=7 if i == 0 else i,
                 crc_verified=True, file_offset=i*4096, crc32=0x12345678)
            for i, n in enumerate(names)]


class UdlPlanTests(unittest.TestCase):
    def test_exact_stage_and_ack_sequence_with_nv_blocked(self):
        t = table()
        before = copy.deepcopy(t)
        p = build_plan(t)
        self.assertEqual(t, before)
        self.assertEqual([s['name'] for s in p['stages']], ['TOC', 'MAIN', 'APM', 'VSS', 'NV'])
        self.assertEqual([s['stage'] for s in p['stages']], [1, 2, 3, 4, 5])
        self.assertEqual(p['stages'][0]['start'], dict(request='00009100', ack='0000a100'))
        main = p['stages'][1]
        self.assertEqual(main['frames'], 2)
        self.assertEqual(main['crc']['request'], '0000920c')
        self.assertEqual(main['done']['ack'], '0000a20d')
        self.assertEqual([s['name'] for s in p['stages'] if s['crc']], ['MAIN'])
        self.assertTrue(p['stages'][-1]['blocked'])
        self.assertIsNone(p['stages'][-1]['file_offset'])
        self.assertTrue(p['finish']['blocked'])

    def test_reject_unreviewed_profile_or_unverified_bytes(self):
        cases = []
        t = table(); t[3], t[4] = t[4], t[3]; cases.append(t)
        t = table(); t[0]['entry_id'] = 8; cases.append(t)
        t = table(); t[2]['crc_verified'] = False; cases.append(t)
        t = table(); t[2]['size'] = 0; cases.append(t)
        t = table(); t[2]['size'] = 1 << 32; cases.append(t)
        cases.extend([table()[:-1], table()+[table()[0]]])
        for t in cases:
            with self.subTest(table=t), self.assertRaises(ValueError):
                build_plan(t)

    def test_stage_numbers_come_from_order_not_reserved_word(self):
        t = table()
        for e in t[1:]:
            e['entry_id'] = 0xffffffff
        self.assertEqual([s['stage'] for s in build_plan(t)['stages']], [1, 2, 3, 4, 5])
