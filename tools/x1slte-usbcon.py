#!/usr/bin/env python3
# x1slte-usbcon.py PORT [COMMAND...]: the phone's USB serial link (usb_gadget=5): reads the kernel log for W seconds
# (env W, default 3), then sends each COMMAND to the line console and prints its "usb" answers. PORT: /dev/cu.usbmodem*

import os, sys, time, termios, tty
fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
def drain(sec):
    out=b''; end=time.time()+sec
    while time.time()<end:
        try:
            b=os.read(fd,65536)
            if b: out+=b; continue
        except BlockingIOError: pass
        time.sleep(0.05)
    return out
first=drain(float(os.environ.get("W","3")))
print("== %d bytes in 3 s; last lines:" % len(first)); print(first.decode(errors='replace')[-1500:])
for cmd in sys.argv[2:]:
    os.write(fd,(cmd+'\r').encode()); r=drain(4).decode(errors='replace')
    print("== reply to %r:" % cmd); print("\n".join(l for l in r.splitlines() if 'usb' in l.lower())[-2500:])
