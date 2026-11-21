import sys,tarfile,struct,lz4.frame
ap,out=sys.argv[1:3]
t=tarfile.open(ap,'r|')
for m in t:
    print(m.name,m.size,flush=True)
    if m.name.startswith('super.img'):
        src=lz4.frame.LZ4FrameFile(t.extractfile(m)) if m.name.endswith('.lz4') else t.extractfile(m)
        def rd(n):
            b=bytearray()
            while len(b)<n:
                c=src.read(n-len(b))
                if not c: raise SystemExit('short')
                b+=c
            return bytes(b)
        h=rd(28);magic,maj,mn,fhs,chs,bs,tb,tc,_=struct.unpack('<IHHHHIIII',h)
        assert magic==0xed26ff3a,hex(magic)
        rd(fhs-28);o=open(out,'wb');o.truncate(tb*bs);pos=0
        for i in range(tc):
            ct,_,cs,ts=struct.unpack('<HHII',rd(12));rd(chs-12);n=cs*bs
            if ct==0xCAC1:
                o.seek(pos*bs);left=n
                while left: c=rd(min(left,1<<24));o.write(c);left-=len(c)
            elif ct==0xCAC2:
                f=rd(4);o.seek(pos*bs);o.write(f*(n//4))
            elif ct==0xCAC4: rd(4)
            pos+=cs
            if i%200==0: print(f'\r{pos*bs>>20} MB',end='',flush=True)
        o.close();print('\ndone',out);break
