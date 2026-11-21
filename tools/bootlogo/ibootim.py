"""iBoot's pictures (IPSW Firmware/all_flash/*.im4p: the battery and plug-in screens) as pixels.

IM4P payload "iBootIm": a 0x40 header (adler32, compression "lzss", format "argb" or "grey", width, height), then
LZSS (4096-byte window, pre-filled with spaces). argb: 4 bytes per pixel, B G R and an unused byte, drawn on black.
grey: 2 bytes per pixel, the value and 255 - coverage. rgb() gives (w, h, RGB bytes drawn on black).
"""
import struct, sys, zlib
def der_payload(b):
    # IM4P: SEQUENCE { IA5 "IM4P", IA5 type, IA5 desc, OCTET STRING payload, ... }
    def hdr(i):
        t = b[i]; l = b[i+1]; i += 2
        if l & 0x80:
            n = l & 0x7f; l = int.from_bytes(b[i:i+n], 'big'); i += n
        return t, l, i
    t, l, i = hdr(0)
    for _ in range(3):
        t, l, j = hdr(i); i = j + l
    t, l, j = hdr(i); assert t == 4
    return b[j:j+l]
def lzss(src, size):
    N, F, TH = 4096, 18, 2
    buf = bytearray(b' ' * (N - F) + bytes(F)); r = N - F; out = bytearray(); i = 0; flags = 0
    while len(out) < size and i < len(src):
        flags >>= 1
        if not flags & 0x100:
            flags = src[i] | 0xff00; i += 1
        if flags & 1:
            c = src[i]; i += 1; out.append(c); buf[r] = c; r = (r + 1) & (N - 1)
        else:
            if i + 1 >= len(src): break
            a, b2 = src[i], src[i+1]; i += 2
            p = a | ((b2 & 0xf0) << 4); n = (b2 & 0x0f) + TH
            for k in range(n + 1):
                c = buf[(p + k) & (N - 1)]; out.append(c); buf[r] = c; r = (r + 1) & (N - 1)
    return bytes(out[:size])
def decode(path):
    b = open(path, 'rb').read()
    p = der_payload(b) if b[0] == 0x30 else b
    assert p[:8] == b'iBootIm\0', p[:8]
    comp, fmt = p[12:16][::-1], p[16:20][::-1]
    w, h = struct.unpack('<HH', p[20:24])
    bpp = 4 if fmt == b'argb' else 2 if fmt == b'grey' else None
    assert comp == b'lzss' and bpp, (comp, fmt)
    return w, h, fmt.decode(), lzss(p[0x40:], w * h * bpp)
def png(path, w, h, fmt, px):
    rows = b''
    for y in range(h):
        bpp = 4 if fmt == 'argb' else 2
        row = px[y*w*bpp:(y+1)*w*bpp]
        if fmt == 'argb':  # stored B,G,R,x: drawn on black, the 4th byte unused
            row = b''.join(bytes((row[k+2], row[k+1], row[k])) for k in range(0, len(row), 4))
        else:              # grey: value, coverage (both stored inverted) -> on black
            row = bytes(row[k] * (255 - row[k+1]) // 255 for k in range(0, len(row), 2))   # value, 255 - coverage
        rows += b'\0' + row
    ct = 2 if fmt == 'argb' else 0
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, ct, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
if __name__ == '__main__':
    for f in sys.argv[1:]:
        w, h, fmt, px = decode(f)
        png(f.replace('.im4p', '.png'), w, h, fmt, px)
        print(f.split('/')[-1], w, h, fmt, len(px))


def rgb(path):
    """(w, h, rgb bytes) on black, whatever the format"""
    w, h, fmt, px = decode(path)
    if fmt == 'argb':
        return w, h, b''.join(bytes((px[k + 2], px[k + 1], px[k])) for k in range(0, len(px), 4))
    return w, h, b''.join(bytes((px[k] * (255 - px[k + 1]) // 255,)) * 3 for k in range(0, len(px), 2))
