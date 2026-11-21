import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('gpio', Path(__file__).resolve().parents[1] / 's5123-gpio.py')
gpio = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gpio)


class GpioTests(unittest.TestCase):
    def test_only_reviewed_addresses_and_three_read_only_words(self):
        for _, _, offset in gpio.BANKS:
            self.assertEqual(gpio.command(gpio.CMGP + offset),
                             f"/var/root/rwsys kern.redwood.poke 'r {gpio.CMGP + offset:#x} 3'")
        for address in (0x13d20000, 0x10960000, 0xf0000000, gpio.CMGP, gpio.CMGP + 0x121):
            with self.subTest(address=address), self.assertRaises(ValueError):
                gpio.command(address)

    def test_physical_level_and_nibble_masks(self):
        result = gpio.decode(0x15c30120, '0x15c30120: fffffff0 ffffffff fffffff3\n')
        self.assertEqual(result['function_name'], 'input')
        self.assertEqual(result['physical_level'], 1)
        self.assertEqual(result['pull_name'], 'up')
        result = gpio.decode(0x15c30120, '15c30120: 00000001 00000000 00000000')
        self.assertEqual(result['function_name'], 'output')
        self.assertEqual(result['physical_level'], 0)

    def test_refuse_wrong_address_short_error_or_trailing_data(self):
        cases = ('0x15c30124: 00000000 00000001 00000001',
                 '0x15c30120: 00000000 00000001', 'unmappable 0x15c30120',
                 '0x15c30120: 00000000 00000001 00000001\nother data')
        for response in cases:
            with self.subTest(response=response), self.assertRaises(ValueError):
                gpio.decode(0x15c30120, response)
