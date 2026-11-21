#!/usr/bin/env python3
import copy
import json
from pathlib import Path
import runpy
import unittest
ROOT=Path(__file__).resolve().parents[2]
PLAN=runpy.run_path(str(Path(__file__).with_name('aux-receiver-plan.py')))['receiver_plan']
CAPTURES=json.loads((ROOT/'docs/devices/x1slte/camera/aux-inventory.json').read_text())
class Routing(unittest.TestCase):
    def test_actual_overlay_routes(self):
        for sensor,instance,dma,phy in [('2la',4,0,0x170d2300),('gw2',2,3,0x170d1b00)]:
            p=PLAN(CAPTURES[0],instance,sensor)
            self.assertEqual(p,PLAN(CAPTURES[1],instance,sensor))
            self.assertEqual((p['phy_mode'],p['trios'],p['dma_engine'],p['phy_base']),('C-PHY',3,dma,phy))
    def test_omitted_gw2_dma_override_is_rejected(self):
        x=copy.deepcopy(CAPTURES[0])
        for p in list(x['overlay_nodes']):
            if p.startswith('/fragment@22/__overlay__/sensor2-ch-mode'):del x['overlay_nodes'][p]
        with self.assertRaisesRegex(ValueError,'DMA engine disagrees'):PLAN(x,2,'gw2')
    def test_phy_name_does_not_select_dphy(self):
        x=copy.deepcopy(CAPTURES[0]);del x['overlay_nodes']['/fragment@24/__overlay__']['use_cphy']
        with self.assertRaisesRegex(ValueError,'explicit C-PHY'):PLAN(x,4,'2la')
    def test_base_only_route_is_rejected(self):
        x=copy.deepcopy(CAPTURES[0]);del x['overlay_nodes']['/fragment@22']
        with self.assertRaisesRegex(ValueError,'explicit board'):PLAN(x,2,'gw2')
if __name__=='__main__':unittest.main()
