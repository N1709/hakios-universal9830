#!/usr/bin/env python3
"""iPhone's own charging pictures for Samsung's bootloader (A, 2026-11-20).

With the phone off, S-Boot shows up_param's lpm.jpg when a charger is plugged in and low_battery_alert.jpg when the
battery is empty. This draws iOS 16.5's (iBoot's) battery screens in their place, from Apple's own art in the IPSW
(Firmware/all_flash/*.im4p, fetched once into sysimage/iboot-art/; never committed):
  out/bootlogo/lpm.jpg                304 x 304   the empty battery with its red sliver and the charging bolt
  out/bootlogo/low_battery_alert.jpg  480 x 480   the empty battery above the plug-in cable
  out/bootlogo/*-preview.png          the same, to look at
Same canvases as Samsung's pictures (S-Boot centers them on black). The installer swaps them like logo.jpg.
"""
import os
import subprocess
import sys
import zlib
import struct

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ibootim  # noqa: E402
import jpeg444  # noqa: E402

ART = {'outline': 'batterylow0@3x~iphone', 'red': 'batterylow1@3x~iphone',
       'bolt': 'batterycharging1@3x~iphone', 'plug': 'glyphplugin@2436~iphone-lightning'}


def fetch(art_dir):
    os.makedirs(art_dir, exist_ok=True)
    for name in ART.values():
        dest = os.path.join(art_dir, name + '.im4p')
        if not os.path.isfile(dest):
            subprocess.run([sys.executable, '-I', os.path.join(TOP, 'sysimage', 'fetch-from-ipsw.py'),
                            'Firmware/all_flash/%s.im4p' % name, dest], check=True, stdout=subprocess.DEVNULL)
    return {k: ibootim.rgb(os.path.join(art_dir, v + '.im4p')) for k, v in ART.items()}


class Canvas:
    def __init__(self, w, h):
        self.w, self.h, self.px = w, h, bytearray(w * h * 3)

    def lighten(self, pic, x0, y0):
        """draw pic (w, h, rgb on black) at x0, y0, keeping the brighter of the two (art on black over art on black)"""
        w, h, p = pic
        for y in range(h):
            ty = y0 + y
            if not 0 <= ty < self.h:
                continue
            for x in range(w):
                tx = x0 + x
                if 0 <= tx < self.w:
                    s, d = 3 * (y * w + x), 3 * (ty * self.w + tx)
                    if sum(p[s:s + 3]) > sum(self.px[d:d + 3]):
                        self.px[d:d + 3] = p[s:s + 3]


def scale(pic, f):
    """area average, for shrinking (f <= 1)"""
    w, h, p = pic
    nw, nh = max(1, round(w * f)), max(1, round(h * f))
    out = bytearray(nw * nh * 3)
    for ny in range(nh):
        y0, y1 = ny * h // nh, max(ny * h // nh + 1, (ny + 1) * h // nh)
        for nx in range(nw):
            x0, x1 = nx * w // nw, max(nx * w // nw + 1, (nx + 1) * w // nw)
            n, acc = (y1 - y0) * (x1 - x0), [0, 0, 0]
            for y in range(y0, y1):
                for x in range(x0, x1):
                    s = 3 * (y * w + x)
                    acc[0] += p[s]; acc[1] += p[s + 1]; acc[2] += p[s + 2]
            out[3 * (ny * nw + nx):3 * (ny * nw + nx) + 3] = bytes(v // n for v in acc)
    return nw, nh, bytes(out)


def battery(art):
    """iBoot's empty battery at its own size (450 x 450): the outline and the red sliver (8 px inside the walls)"""
    c = Canvas(450, 450)
    c.lighten(art['outline'], 0, 0)
    c.lighten(art['red'], 39 + 8, 0)
    return 450, 450, bytes(c.px)


def lpm(art, size=304):
    """charging: the battery with Apple's glowing bolt over it"""
    c = Canvas(450, 450)
    c.lighten(battery(art), 0, 0)
    bolt = scale(art['bolt'], 0.42)                       # the bolt (~315 of 536 rows) about the battery's height
    c.lighten(bolt, (450 - bolt[0]) // 2, (450 - bolt[1]) // 2 + 2)
    return scale((450, 450, bytes(c.px)), size / 450)


def low(art, size=480):
    """battery empty: the battery, and under it the cable to plug in"""
    c = Canvas(size, size)
    bat = scale(battery(art), 0.8)                         # 360 x 360, the battery itself in rows ~114..245
    c.lighten(bat, (size - bat[0]) // 2, -60)
    plug = scale(art['plug'], 0.26)                        # 22 x 257
    c.lighten(plug, (size - plug[0]) // 2, size - plug[1] - 10)
    return size, size, bytes(c.px)


def png(path, pic):
    w, h, p = pic
    raw = b''.join(b'\0' + p[3 * w * y:3 * w * (y + 1)] for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def main():
    art = fetch(os.path.join(TOP, 'sysimage', 'iboot-art'))
    od = os.path.join(TOP, 'out', 'bootlogo')
    os.makedirs(od, exist_ok=True)
    for name, pic in (('lpm', lpm(art)), ('low_battery_alert', low(art))):
        open(os.path.join(od, name + '.jpg'), 'wb').write(jpeg444.encode_rgb(*pic))
        png(os.path.join(od, name + '-preview.png'), pic)
        print('mkcharging: out/bootlogo/%s.jpg (%dx%d, iPhone art)' % (name, pic[0], pic[1]))


if __name__ == '__main__':
    main()
