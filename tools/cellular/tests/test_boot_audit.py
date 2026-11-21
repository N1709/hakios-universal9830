import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('boot_audit', Path(__file__).parents[1] / 'boot-audit.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

MANIFEST = dict(cp_boot=dict(size=65), installed_firmware_preserved=True,
                installed_loader_fallback=True, avb_verified=True)
REPORT = '''== CELLULAR CP BOOT PREFLIGHT (one guarded power/BOOT attempt) ==
cellular memory: reserved 3/3 (UEFI ownership only)
cellular memory: normal-nc 3/3 (DXE and live EL1 translation; every CP page)
cellular MMIO: preflight result 1
cellular SPI loopback: preflight result 1
cellular GPIO: preflight result 1
cellular CP BOOT: SPI pad ownership verified 1
cellular CP BOOT: PIO error 0 queued 65 completed 65 packets 2 status 2000000
cellular CP BOOT: SPI/clock/pad/AP ownership restored 1; power requested 1
cellular CP BOOT: error 0 phase 6 completed 65 cleanup safe 1; CP acceptance unproven
cellular preflight: handing off to the original loader
'''


class BootAuditTests(unittest.TestCase):
    def test_controller_success_does_not_claim_service_or_restore(self):
        result = module.audit(REPORT, MANIFEST)
        self.assertTrue(result['controller_transfer_and_shutdown_pass'])
        for key in ('cp_acceptance', 'sim_detection', 'lte', 'volte'):
            self.assertEqual(result[key], 'unproven')
        self.assertIn('independent', result['boot_partition_restored'])

    def test_failures_and_missing_evidence(self):
        for old, new in (('completed 65', 'completed 64'), ('cleanup safe 1', 'cleanup safe 0'),
                         ('PIO error 0', 'PIO error -2'), ('packets 2', 'packets 1'),
                         ('phase 6', 'phase 5'), ('GPIO: preflight result 1', 'GPIO: preflight result 0'),
                         ('cellular preflight: handing off to the original loader', '')):
            with self.subTest(old=old):
                self.assertFalse(module.audit(REPORT.replace(old,new), MANIFEST)['controller_transfer_and_shutdown_pass'])

    def test_recovered_hold_still_requires_review(self):
        self.assertFalse(module.audit(REPORT+'cellular CP BOOT: HOLD; cleanup unconfirmed\n', MANIFEST)['controller_transfer_and_shutdown_pass'])

    def test_mixed_or_missing_boot_rejected(self):
        for report in ('', REPORT + REPORT):
            with self.assertRaises(ValueError):
                module.audit(report, MANIFEST)
        with self.assertRaises(ValueError):
            module.audit(REPORT, {})


if __name__ == '__main__':
    unittest.main()
