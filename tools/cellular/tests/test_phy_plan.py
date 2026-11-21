import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('phy_plan', Path(__file__).parents[1] / 'phy-plan.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def source(body):
    return '} else if (chip_ver == 1) {' + body + '\n}'


class PhyPlanTests(unittest.TestCase):
    def test_preserves_order_repetition_and_lane_offset(self):
        ops = module.extract(source('''
        /* writel(0xff, phy_base_regs + 0x0); */
        writel(0x0a, phy_base_regs + 0xc70);
        writel(0x00, phy_base_regs + 0xc70);
        writel(0x08, phy_base_regs + 0x82c + 0x800);
        '''))
        self.assertEqual(ops, [dict(offset=0xc70,value=10), dict(offset=0xc70,value=0),
                               dict(offset=0x102c,value=8)])

    def test_unknown_or_out_of_range_operations_refused(self):
        for body in ('writel(value, phy_base_regs + 0x18);',
                     'writel(0x1, phy_base_regs + 0x2000);',
                     'writel(0x1, phy_base_regs + 0x19);',
                     'writel(0x1, sysreg_base_regs + 0x18);', 'udelay(5);', ''):
            with self.subTest(body=body), self.assertRaises(ValueError): module.extract(source(body))
