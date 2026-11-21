#!/usr/bin/env python3
"""Read the ISIM identity (IMPI, home domain, IMPU) through redwood-cellulard's `apdu` command.
Prints identities masked; with --save PATH writes them unmasked to a local private file. MIT."""
import subprocess,sys,time,re
def ph(cmd): return subprocess.run(['ssh','s20',cmd],capture_output=True,text=True,timeout=40).stdout
def apdu(h):
    ph(f"echo 'apdu {h}' > /var/tmp/redwood-cell/cmd")
    for _ in range(20):
        time.sleep(0.4);r=ph("cat /var/tmp/redwood-cell/apdu 2>/dev/null").strip()
        if r: return bytes.fromhex(r)
    return b''
def show(tag,b): print(f"{tag}: {len(b)} bytes, head {b[:4].hex()} tail {b[-4:].hex()}")
r=apdu("0070000001");show("open channel",r)
m=re.search(rb'\x90\x00',r)
# the answer layout is not known yet: print its shape only, decide the channel from the byte before 90 00
ch=r[m.start()-1] if m and m.start()>0 else 0
print("channel",ch)
if not ch: sys.exit(1)
c=f"{ch:02x}" if ch<4 else f"{0x40+ch-4:02x}"
r=apdu(f"{c}a4040407a0000000871004");show("select ISIM",r)
def txt(b):
    s=b.decode('latin1');return re.sub(r'[^ -~]','.',s)
def mask(s): return re.sub(r'\d{6,}',lambda m:m.group(0)[:5]+'*'*(len(m.group(0))-5),s)
for name,sel,rd in (("IMPI","6f02",f"{c}b0000000"),("DOMAIN","6f03",f"{c}b0000000"),("IMPU1","6f04",f"{c}b2010400"),("IMPU2","6f04",f"{c}b2020400")):
    r=apdu(f"{c}a40004026{sel[1:]}" if False else f"{c}a4000402{sel}");rr=apdu(rd)
    print(f"{name}: select {r[-2:].hex() if r else '-'} read {len(rr)} bytes:",mask(txt(rr)))
r=apdu(f"0070800{ch:x}00" if ch<16 else "");show("close",r)
