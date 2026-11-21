"""Read Apple's CgBI PNG (iOS "optimised" PNG: BGRA, premultiplied, raw deflate) into (width, height, coverage bytes).
The boot logo is one colour, so only its coverage (alpha, or luminance on an opaque image) is kept."""
import struct
import zlib


def read(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    i, cgbi, idat, w = 8, False, b'', None
    while i < len(d):
        n, t = struct.unpack('>I4s', d[i:i + 8])
        body = d[i + 8:i + 8 + n]
        if t == b'CgBI':
            cgbi = True
        elif t == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
            assert depth == 8 and ctype in (6, 2, 0, 4) and interlace == 0, (depth, ctype, interlace)
        elif t == b'IDAT':
            idat += body
        i += 12 + n
    raw = zlib.decompress(idat, -15) if cgbi else zlib.decompress(idat)
    bpp = {6: 4, 2: 3, 0: 1, 4: 2}[ctype]
    stride = w * bpp
    out, prev, p = bytearray(), bytearray(stride), 0
    for _ in range(h):
        f = raw[p]; line = bytearray(raw[p + 1:p + 1 + stride]); p += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line; prev = line
    cov = bytearray(w * h)
    for k in range(w * h):
        px = out[k * bpp:(k + 1) * bpp]
        if ctype == 6:
            cov[k] = px[3] if px[3] < 255 or True else 255
            # opaque image with a white logo on black: use luminance instead
        elif ctype == 2:
            cov[k] = max(px)
        elif ctype == 0:
            cov[k] = px[0]
        else:
            cov[k] = px[1]
    if ctype == 6 and min(cov) == 255:          # opaque RGBA: luminance
        for k in range(w * h):
            cov[k] = max(out[k * 4:k * 4 + 3])
    return w, h, bytes(cov)
