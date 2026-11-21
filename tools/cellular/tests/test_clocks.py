import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('clocks', Path(__file__).parents[1] / 's5123-clocks.py')
clocks = importlib.util.module_from_spec(spec)
spec.loader.exec_module(clocks)


class ClockTests(unittest.TestCase):
    def test_reviewed_single_word_reads_only(self):
        self.assertEqual(len(clocks.REGISTERS), 13)
        self.assertEqual(clocks.REGISTERS['gpp8_con'], 0x10730060)
        self.assertEqual(clocks.REGISTERS['pcie1_phy_control'], 0x15860720)
        self.assertEqual(clocks.REGISTERS['usi_mode'], 0x10721048)
        for address in clocks.REGISTERS.values():
            self.assertTrue(clocks.command(address).endswith(f"'r {address:#x} 1'"))
        for address in (0x10960000, 0x10721028, 0x13d20000):
            with self.subTest(address=address), self.assertRaises(ValueError):
                clocks.command(address)
        self.assertEqual(clocks.word(0x10700690, '0x10700690: 00010010\n'), 0x10010)
        for text in ('0x10700694: 00000000', '0x10700690: 00000000 00000000', 'ERROR'):
            with self.assertRaises(ValueError):
                clocks.word(0x10700690, text)

    def test_clock_rate_bound_and_unknown_fast_parent(self):
        plan = clocks.safe_oscillator_plan()
        self.assertEqual(plan['divisor'], 4)
        self.assertEqual(plan['sclk_hz'], 1_625_000)
        values = dict.fromkeys(clocks.REGISTERS, 0)
        values.update(usi_mode=2, divider=2)
        decoded = clocks.decode(values)
        self.assertEqual(decoded['usi_mode'], 'spi')
        self.assertTrue(decoded['pins']['gpp8-6']['shared_with_dump_notification'])
        self.assertTrue(decoded['pcie1_phy_isolation_asserted'])
        self.assertEqual(decoded['ap2cp_wakeup']['pin'], 'gpg1-3')
        self.assertFalse(decoded['estimated_within_boot_limit'])  # 26/3/4 MHz exceeds 2.15 MHz.
        values['divider']=3
        self.assertTrue(clocks.decode(values)['estimated_within_boot_limit'])
        values['mux']=16
        self.assertNotIn('estimated_sclk_hz', clocks.decode(values))
        values['mux']=1 << 16
        self.assertNotIn('estimated_sclk_hz', clocks.decode(values))
        del values['usi_mode']
        with self.assertRaises(ValueError):
            clocks.decode(values)


if __name__ == '__main__':
    unittest.main()
