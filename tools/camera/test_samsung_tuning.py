#!/usr/bin/env python3
"""Synthetic tests: no stock firmware or captured images required."""
import copy
import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def module(name, file):
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


tuning = module('tuning', 'inspect-samsung-tuning.py')
recovery = module('recovery', 'recover-samsung-ddk-schema.py')


class TuningTests(unittest.TestCase):
    def setUp(self):
        self.schema = tuning.load_schema()
        self.payload = bytearray((self.schema['serialized_bytes'] + 8 + 15) & ~15)
        struct.pack_into('<II', self.payload, 0, int(self.schema['payload_magic'], 0),
                         self.schema['serialized_bytes'])

    def test_schema_and_split(self):
        blocks = tuning.split_payload(self.payload, self.schema)
        self.assertEqual(len(blocks), 65)
        self.assertEqual(sum(map(len, blocks.values())), 322097)
        self.assertEqual(len(blocks['ccmTuning']), 8436)

    def test_split_rejects_bad_header_size_padding(self):
        cases = [b'', self.payload[:7], self.payload[:-1], self.payload + b'\0']
        for offset, value in ((0, 1), (4, 1), (len(self.payload)-1, 1)):
            data = bytearray(self.payload)
            if offset == len(data)-1:
                data[offset] = value
            else:
                struct.pack_into('<I', data, offset, value)
            cases.append(data)
        for i, data in enumerate(cases):
            with self.subTest(case=i), self.assertRaises(ValueError):
                tuning.split_payload(data, self.schema)

    def test_schema_rejects_gaps_duplicates_and_wrong_total(self):
        for mutation in ('offset', 'key', 'size', 'total'):
            schema = copy.deepcopy(self.schema)
            if mutation == 'total':
                schema['serialized_bytes'] += 1
            elif mutation == 'key':
                schema['blocks'][1]['key'] = schema['blocks'][0]['key']
            else:
                schema['blocks'][1][mutation] = 0
            with tempfile.TemporaryDirectory() as directory:
                import json
                path = Path(directory) / 'schema.json'
                path.write_text(json.dumps(schema))
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    tuning.load_schema(path)

    def test_ccm_signed_coefficients_and_record_bounds(self):
        data = bytearray(0x20f4)
        struct.pack_into('<9h', data, 0x78, 300, -20, -24, 0, 256, 0, 0, 0, 256)
        struct.pack_into('<6H', data, 8, 1, 2, 3, 4, 5, 6)
        result = tuning.decode_ccm(data, True)
        self.assertEqual(len(result['records']), 34)
        self.assertEqual(result['awbLowlightCcmCord'], (1, 2, 3, 4, 5, 6))
        pre = result['records'][0]['PreCCM']
        self.assertEqual(pre['signed_coefficients'][1], -20)
        self.assertEqual(pre['row_sums'], [256, 256, 256])
        self.assertFalse(pre['identity_256'])
        self.assertEqual(result['records'][-1]['offset'] + 0xf4, 0x20e0)

    def test_ccm_default_omits_values(self):
        result = tuning.decode_ccm(bytes(0x20f4))
        self.assertNotIn('signed_coefficients', result['records'][0]['PreCCM'])
        self.assertNotIn('VectorsHigh', result['records'][0])

    def test_lut_arrays_and_tail_offsets(self):
        data = bytearray(0x67a4)
        struct.pack_into('<H', data, 0, 6600)
        for i in range(4):
            struct.pack_into('<h', data, 2 + 6600*i, -300 + i)
            struct.pack_into('<h', data, 2 + 6600*i + 6598, 400 + i)
        struct.pack_into('<I', data, 0x6724, 123)
        struct.pack_into('<I', data, 0x6764 + 15*4, 456)
        result = tuning.decode_gras_lut(data, True)
        self.assertEqual(result['lut']['signed_values'][0], -300)
        self.assertEqual(result['lutColorTempDiff']['signed_values'][-1], 403)
        self.assertEqual(result['lutWeight'][0], 123)
        self.assertEqual(result['lutTh'][-1], 456)

    def test_decoder_rejects_wrong_shape(self):
        for decoder in (tuning.decode_ccm, tuning.decode_gras_lut):
            with self.assertRaises(ValueError):
                decoder(b'bad')
        with self.assertRaises(ValueError):
            tuning.decode_gras_lut(bytes(0x67a4))

    def test_main_gras_grids_are_unsigned_planar_and_distinct(self):
        data = bytearray(0xa3d4)
        for grid in range(6):
            at = 0x920 if grid == 0 else 0x22e8 + (grid-1)*6600
            for plane in range(4):
                struct.pack_into('<H', data, at + plane*825*2, 1000 + grid*10 + plane)
                struct.pack_into('<H', data, at + (plane*825+412)*2, 128)
        result = tuning.decode_gras(data, True)
        self.assertEqual(result['grid_shape'], [4, 25, 33])
        self.assertEqual(len(result['grids']), 6)
        self.assertEqual(result['grids'][0]['planes'][3]['unsigned_values'][0], 1003)
        self.assertEqual(result['grids'][5]['planes'][2]['centre'], 128)
        self.assertEqual(result['grids'][5]['planes'][2]['corners'][0], 1052)
        self.assertEqual(result['grids'][-1]['offset'] + 6600, 0xa3d0)

    def test_main_gras_rejects_incomplete_block(self):
        with self.assertRaises(ValueError):
            tuning.decode_gras(bytes(0xa3d3))

    def test_ccm_values_include_reconstructed_helper_only(self):
        result = tuning.decode_ccm(bytes(0x20f4), True)
        self.assertEqual(len(result['records'][0]['ccm9_helper_with_PreCCM_q10']), 9)
        self.assertNotIn('ccm9_helper_with_PreCCM_q10', tuning.decode_ccm(bytes(0x20f4))['records'][0])

    def test_blc_signed_values_and_capacity(self):
        data = bytearray(0x180)
        struct.pack_into('<4h', data, 0xc, -100, -200, -300, -400)
        struct.pack_into('<I', data, 0x9c, 9)
        struct.pack_into('<4h', data, 0xa0 + 8*8, -1, -2, -3, -4)
        struct.pack_into('<I', data, 0x134, 9)
        struct.pack_into('<4H', data, 0x138 + 8*8, 100, 200, 300, 400)
        result = tuning.decode_blc(data, True)
        self.assertEqual(result['offsetsTableFixed'], (-100, -200, -300, -400))
        self.assertEqual(result['offsetTableVariable']['values'][-1], (-1, -2, -3, -4))
        self.assertEqual(result['gainTableVariable']['values'][-1], (100, 200, 300, 400))
        struct.pack_into('<I', data, 0x134, 10)
        with self.assertRaises(ValueError):
            tuning.decode_blc(data)
        with self.assertRaises(ValueError):
            tuning.decode_blc(b'bad')

    def test_complete_container_to_named_tuning(self):
        lut = next(b for b in self.schema['blocks'] if b['key'] == 'grasLutTuning')
        struct.pack_into('<H', self.payload, lut['offset'], 6600)
        # One scenario, one IP, one entry, payload at absolute file offset 44.
        header = struct.pack('<II4s4sIII', tuning.container.MAGIC, 1 << 3,
                             b'ISV8', b'TEST', 1, 1, 44)
        tables = struct.pack('<4I', 0, 1, 0, len(self.payload))
        result = tuning.inspect(header + tables + self.payload, self.schema,
                                scenarios={0: ['SYNTHETIC_PREVIEW']})
        entry = result['entries'][0]
        self.assertEqual(entry['scenario_names'], {'0': ['SYNTHETIC_PREVIEW']})
        self.assertEqual(len(entry['blocks']), 65)
        self.assertEqual(entry['blocks'][37]['offset'], 44 + 0x29648)
        self.assertEqual(len(entry['ccm']['records']), 34)
        self.assertIn('blcStrp', entry['blc'])

    def test_source_enum_aliases_and_increment(self):
        names = tuning.scenario_names('''enum is_subscenario_id {
            ISS_SUB_SCENARIO_TEST = 0, /* preview */
            ISS_SUB_SCENARIO_NEXT, ISS_SUB_SCENARIO_ALIAS = ISS_SUB_SCENARIO_TEST,
            ISS_SUB_END,
        };''')
        self.assertEqual(len(names[0]), 2)
        self.assertEqual(names[1], ['ISS_SUB_SCENARIO_NEXT'])

    def test_enum_rejects_unknown_expressions(self):
        for source in ('not an enum', 'enum is_subscenario_id { ISS_SUB_SCENARIO_A = 1 << 4 };'):
            with self.assertRaises(ValueError):
                tuning.scenario_names(source)

    def test_unknown_ddk_rejected_before_disassembly(self):
        with self.assertRaises(ValueError):
            recovery.recover(b'unknown firmware')


if __name__ == '__main__':
    unittest.main()
