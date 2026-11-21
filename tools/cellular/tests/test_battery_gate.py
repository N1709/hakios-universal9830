import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('battery_gate', Path(__file__).parents[1] / 'battery-gate.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
SAMPLE = '''"CurrentCapacity" = 10
"MaxCapacity" = 100
"Voltage" = 3800
"Amperage" = 711
"IsCharging" = Yes
"ExternalConnected" = Yes
'''


class BatteryGateTests(unittest.TestCase):
    def test_ready_and_scaled_capacity(self):
        self.assertTrue(module.decode(SAMPLE)['ready'])
        scaled = SAMPLE.replace('Capacity" = 10\n', 'Capacity" = 400\n').replace('Capacity" = 100\n', 'Capacity" = 4000\n')
        self.assertEqual(module.decode(scaled)['percent'], 10)

    def test_low_charge_voltage_and_disconnect_refused(self):
        for old,new in (('= 10\n','= 5\n'), ('3800','3687'), ('711','-100'),
                        ('IsCharging" = Yes','IsCharging" = No'),
                        ('ExternalConnected" = Yes','ExternalConnected" = No')):
            self.assertFalse(module.decode(SAMPLE.replace(old,new))['ready'])

    def test_missing_or_duplicate_refused(self):
        for sample in ('', SAMPLE + '"Voltage" = 3900\n'):
            with self.assertRaises(ValueError): module.decode(sample)

    def test_near_full_charge_can_idle_without_being_rejected(self):
        full = SAMPLE.replace('Capacity" = 10\n', 'Capacity" = 96\n').replace('711','0').replace('IsCharging" = Yes','IsCharging" = No')
        self.assertTrue(module.decode(full)['ready'])
        self.assertFalse(module.decode(full.replace('= 96\n','= 20\n'))['ready'])
