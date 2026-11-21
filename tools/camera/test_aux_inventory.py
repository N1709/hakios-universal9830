#!/usr/bin/env python3
"""Fixup safety tests, without firmware or device dependencies."""
import runpy
import unittest
from unittest.mock import patch
from pathlib import Path

M = runpy.run_path(str(Path(__file__).with_name('aux-inventory.py')))


class Fixups(unittest.TestCase):
    def test_external_bus_is_symbolic(self):
        base = {'/__symbols__': {'bus': b'/hsi2c@10560000\0'}}
        overlay = {'/__fixups__': {'bus': b'/fragment@16:target:0\0'},
                   '/fragment@16': {'target': b'\xff'*4}}
        self.assertEqual(M['fixups'](base, overlay)[('/fragment@16', 'target', 0)],
                         {'symbol': 'bus', 'base_path': '/hsi2c@10560000'})

    def test_missing_symbol_preserved(self):
        overlay = {'/__fixups__': {'unknown': b'/f:target:0\0'},
                   '/f': {'target': b'\xff'*4}}
        self.assertIsNone(M['fixups']({}, overlay)[('/f', 'target', 0)]['base_path'])

    def test_controller_and_supply_metadata_are_retained(self):
        root='/fragment@19/__overlay__/is-2la@2D'
        base={'/__symbols__': {'bus': b'/hsi2c@15D70000\0'}, '/hsi2c@15D70000': {}}
        overlay={
            '/__fixups__': {'bus': b'/fragment@19:target:0\0'},
            '/fragment@19': {'target': b'\xff'*4},
            '/fragment@19/__overlay__': {'clock-frequency': (1000000).to_bytes(4,'big')},
            root: {'reg': (45).to_bytes(4,'big')},
            root+'/power_seq/scenario_normal_on/20': {'pname': b'VDDA_2.8V_WIDESUB\0'},
            '/pmic/regulators/ldo12': {'regulator-name': b'VDDA_2.8V_WIDESUB\0',
                'regulator-min-microvolt': (2800000).to_bytes(4,'big')},
            '/pmic/regulators/unrelated': {'regulator-name': b'unrelated\0'}}
        base['/__symbols__']['is_sensor2']=b'/is_sensor2@17050000\0'
        base['/is_sensor2@17050000']={}
        overlay['/__fixups__']['is_sensor2']=b'/fragment@22:target:0\0'
        overlay['/fragment@22']={'target':b'\xff'*4}
        overlay['/fragment@22/__overlay__']={'use_cphy':(1).to_bytes(4,'big'),'dma_ch':(3).to_bytes(4,'big')}
        with patch.dict(M['inventory'].__globals__, {'NODES': lambda b: base if b==b'base' else overlay}):
            result=M['inventory'](b'base',b'overlay')['overlay_nodes']
        self.assertEqual(result['/fragment@19/__overlay__']['clock-frequency'],[1000000])
        self.assertEqual(result['/pmic/regulators/ldo12']['regulator-min-microvolt'],[2800000])
        self.assertNotIn('/pmic/regulators/unrelated',result)
        self.assertEqual(result['/fragment@22/__overlay__']['use_cphy'],[1])
        self.assertEqual(result['/fragment@22/__overlay__']['dma_ch'],[3])

    def test_invalid_offsets_rejected(self):
        for offset in (-4, 1, 4):
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                M['fixups']({}, {'/__fixups__': {'bus': f'/f:target:{offset}\0'.encode()},
                                  '/f': {'target': b'\xff'*4}})


if __name__ == '__main__':
    unittest.main()
