"""A small baseline JPEG writer for grey pictures, 3 components at 4:4:4 (Y, Cb=Cr=128): the format of Samsung's own
up_param images (S-Boot's decoder is only known to take what Samsung ships). Standard Annex K tables. Blocks of one
flat value (nearly all of a logo on black) are coded without a DCT."""
import math

ZIGZAG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14,
          21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60,
          61, 54, 47, 55, 62, 63]
QL = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51,
      87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101,
      72, 92, 95, 98, 112, 100, 103, 99]
QC = [17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99,
      99, 99, 99] + [99] * 32
DC_L = ([0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0], list(range(12)))
DC_C = ([0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0], list(range(12)))
AC_L = ([0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d], bytes.fromhex(
    '01020300041105122131410613516107227114328191a1082342b1c11552d1f02433627282090a161718191a25262728292a3435363738393a'
    '434445464748494a535455565758595a636465666768696a737475767778797a838485868788898a92939495969798999aa2a3a4a5a6a7a8a9'
    'aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9fa'))
AC_C = ([0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77], bytes.fromhex(
    '000102031104052131061241510761711322328108144291a1b1c109233352f0156272d10a162434e125f11718191a262728292a35363738'
    '393a434445464748494a535455565758595a636465666768696a737475767778797a82838485868788898a92939495969798999aa2a3a4a5a6'
    'a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9fa'))


def scaled(q, quality):
    s = 5000 // quality if quality < 50 else 200 - 2 * quality
    return [max(1, min(255, (v * s + 50) // 100)) for v in q]


def codes(spec):
    counts, syms = spec
    table, code, k = {}, 0, 0
    for length in range(1, 17):
        for _ in range(counts[length - 1]):
            table[syms[k]] = (code, length); code += 1; k += 1
        code <<= 1
    return table


class Bits:
    def __init__(self):
        self.out, self.acc, self.n = bytearray(), 0, 0

    def put(self, code, length):
        self.acc = (self.acc << length) | code; self.n += length
        while self.n >= 8:
            self.n -= 8; b = (self.acc >> self.n) & 255; self.out.append(b)
            if b == 255:
                self.out.append(0)
        self.acc &= (1 << self.n) - 1

    def flush(self):
        if self.n:
            self.put((1 << (8 - self.n)) - 1, 8 - self.n)


def magnitude(v):
    a, n = abs(v), 0
    while a:
        n += 1; a >>= 1
    return n, (v if v >= 0 else v + (1 << n) - 1)


C = [[(math.sqrt(0.125) if u == 0 else 0.5) * math.cos((2 * x + 1) * u * math.pi / 16) for x in range(8)] for u in range(8)]


def fdct(block):
    tmp = [[sum(C[u][x] * block[y * 8 + x] for x in range(8)) for u in range(8)] for y in range(8)]
    return [sum(C[v][y] * tmp[y][u] for y in range(8)) for v in range(8) for u in range(8)]


def encode_rgb(w, h, rgb, quality=95):
    """rgb: bytes of w*h*3; a colour picture (the charging screens' red battery), same 4:4:4 layout"""
    y, cb, cr = bytearray(w * h), bytearray(w * h), bytearray(w * h)
    for i in range(w * h):
        r, g, b = rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]
        y[i] = max(0, min(255, round(0.299 * r + 0.587 * g + 0.114 * b)))
        cb[i] = max(0, min(255, round(128 - 0.168736 * r - 0.331264 * g + 0.5 * b)))
        cr[i] = max(0, min(255, round(128 + 0.5 * r - 0.418688 * g - 0.081312 * b)))
    return encode(w, h, bytes(y), quality, bytes(cb), bytes(cr))


def encode(w, h, grey, quality=95, cb=None, cr=None):
    """grey: bytes of w*h (the Y plane; with cb and cr a colour picture); returns JPEG bytes"""
    ql, qc = scaled(QL, quality), scaled(QC, quality)
    hdc_l, hac_l, hdc_c, hac_c = codes(DC_L), codes(AC_L), codes(DC_C), codes(AC_C)
    bits, pred = Bits(), [0, 0, 0]

    def block_coeffs(vals, q):
        flat = vals[0]
        if all(v == flat for v in vals):
            out = [0] * 64; out[0] = int(round((flat - 128) * 8 / q[0])); return out
        f = fdct([v - 128 for v in vals])
        return [int(round(f[ZIGZAG[k]] / q[ZIGZAG[k]])) for k in range(64)]

    def emit(coeffs, comp, hdc, hac):
        d = coeffs[0] - pred[comp]; pred[comp] = coeffs[0]
        n, b = magnitude(d); bits.put(*hdc[n])
        if n:
            bits.put(b, n)
        run = 0
        for k in range(1, 64):
            v = coeffs[k]
            if v == 0:
                run += 1; continue
            while run > 15:
                bits.put(*hac[0xF0]); run -= 16
            n, b = magnitude(v); bits.put(*hac[(run << 4) | n]); bits.put(b, n); run = 0
        if run:
            bits.put(*hac[0x00])

    chroma = [0] * 64

    def block(plane, bx, by):
        vals = []
        for y in range(8):
            row = min(h - 1, by + y) * w
            for x in range(8):
                vals.append(plane[row + min(w - 1, bx + x)])
        return vals
    for by in range(0, h, 8):
        for bx in range(0, w, 8):
            emit(block_coeffs(block(grey, bx, by), ql), 0, hdc_l, hac_l)
            emit(chroma if cb is None else block_coeffs(block(cb, bx, by), qc), 1, hdc_c, hac_c)
            emit(chroma if cr is None else block_coeffs(block(cr, bx, by), qc), 2, hdc_c, hac_c)
    bits.flush()

    def seg(marker, body):
        return bytes((0xFF, marker)) + (len(body) + 2).to_bytes(2, 'big') + body

    def dht(cls_id, spec):
        return bytes((cls_id,)) + bytes(spec[0]) + bytes(spec[1])
    out = b'\xff\xd8' + seg(0xE0, b'JFIF\0\x01\x01\0\0\x01\0\x01\0\0')
    out += seg(0xDB, b'\x00' + bytes(ql[ZIGZAG[k]] for k in range(64)) + b'\x01' + bytes(qc[ZIGZAG[k]] for k in range(64)))
    out += seg(0xC0, bytes((8,)) + h.to_bytes(2, 'big') + w.to_bytes(2, 'big') + bytes((3, 1, 0x11, 0, 2, 0x11, 1, 3, 0x11, 1)))
    out += seg(0xC4, dht(0x00, DC_L) + dht(0x10, AC_L) + dht(0x01, DC_C) + dht(0x11, AC_C))
    out += seg(0xDA, bytes((3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0)))
    return out + bytes(bits.out) + b'\xff\xd9'
