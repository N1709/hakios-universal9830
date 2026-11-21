#!/usr/bin/env python3
import runpy
from pathlib import Path
import unittest
S=runpy.run_path(str(Path(__file__).with_name('catalogue-aux-setfiles.py')))['summarize']
class Modes(unittest.TestCase):
    def test_page_is_respected(self):
        values=[0xfcfc,0x4000,2,0x034c,4032,2,0x034e,3024,2,
                0xfcfc,0x2000,2,0x034c,7,2]
        self.assertEqual(S('sensor_2la_setfile_A_4032x3024_30fps',values)['page4000_registers']['width'],4032)
    def test_mislabeled_mode_rejected(self):
        with self.assertRaises(ValueError):S('sensor_gw2_setfile_A_9248x6936_15fps',[0xfcfc,0x4000,2,0x034c,4032,2,0x034e,3024,2])
    def test_partial_triplets_rejected(self):
        with self.assertRaises(ValueError):S('broken',[0xfcfc,0x4000])
if __name__=='__main__':unittest.main()
