"""Offline reconstruction of the DDK's ConvertVectorsCcm9ToMatrices (0x2b4938).

This reproduces one helper, not the whole stock CCM pipeline. Inputs are 27
signed vector coordinates and a row-major signed Q8 3x3 transform. Output is
nine sectors, each storing two columns (three rows each) in signed Q10.
"""


def signed(value, bits):
    return (value + (1 << (bits - 1))) % (1 << bits) - (1 << (bits - 1))


def ccm9_to_matrices(vectors, pre):
    if len(vectors) != 27 or len(pre) != 9:
        raise ValueError('CCM9 requires 27 vector coordinates and 9 matrix coefficients')
    if any(not isinstance(v, int) or not -32768 <= v <= 32767 for v in (*vectors, *pre)):
        raise ValueError('CCM9 inputs must be signed 16-bit integers')
    result = []
    for sector in range(9):
        transform = []
        for row in range(3):
            a = vectors[sector * 3 + row]
            b = vectors[(sector + 1) * 3 + row] if sector < 8 else vectors[row]
            rows = (
                (a, 2*(b-a), 256+a-2*b),
                (2*a-b, 2*(b-a), 256-b),
                (2*(a-b), 2*b-a, 256-a),
                (2*(a-b), b, 256+b-2*a),
                (256-b, a, b-a),
                (256-a, 2*(a-b), 2*b-a),
                (256+b-2*a, 2*(a-b), b),
                (b-a, 256-b, a),
                (b, 256-a, a-b),
            )
            transform.append(tuple(signed(v, 16) for v in rows[sector]))
        packed = []
        for column in range(2):
            for row in range(3):
                total = signed(sum(transform[row][j] * pre[j*3 + column] for j in range(3)), 32)
                # Stock 0x2b4a18..34: signed nearest, ties away from zero, /64.
                value = (abs(total) + 32) // 64
                packed.append(signed(-value if total < 0 else value, 16))
        result.append(packed)
    return result
