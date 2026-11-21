#!/usr/bin/env python3
"""Synthetic properties of the reconstructed stock CCM9 helper."""
import unittest
from samsung_ccm_math import ccm9_to_matrices

# The helper's nine colour-region boundary coordinates, synthetic identity case.
IDENTITY_VECTORS = (256, 0, 0, 256, 128, 0, 256, 256, 0, 128, 256, 0,
                    0, 256, 0, 0, 256, 256, 0, 128, 256, 0, 0, 256, 256, 0, 256)
IDENTITY = (256, 0, 0, 0, 256, 0, 0, 0, 256)


class CCM9Tests(unittest.TestCase):
    def test_identity_in_all_sectors(self):
        self.assertEqual(ccm9_to_matrices(IDENTITY_VECTORS, IDENTITY),
                         [[1024, 0, 0, 0, 1024, 0]] * 9)

    def test_matrix_composition_and_column_packing(self):
        pre = (300, -40, -4, -60, 330, -14, 10, -30, 276)
        self.assertEqual(ccm9_to_matrices(IDENTITY_VECTORS, pre),
                         [[1200, -240, 40, -160, 1320, -120]] * 9)

    def test_invalid_inputs(self):
        for vectors, pre in (((), IDENTITY), (IDENTITY_VECTORS, ()),
                             ((40000,) * 27, IDENTITY), ((1.2,) * 27, IDENTITY)):
            with self.assertRaises(ValueError):
                ccm9_to_matrices(vectors, pre)

    def test_zero_input_matrix(self):
        self.assertEqual(ccm9_to_matrices(IDENTITY_VECTORS, [0]*9), [[0]*6]*9)


if __name__ == '__main__':
    unittest.main()
