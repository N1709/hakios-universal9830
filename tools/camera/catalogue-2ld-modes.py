#!/usr/bin/env python3
"""Inventory Samsung S5K2LD modes and timing; never enables/advertises modes.
SSM table labels describe special capture machinery, not normal delivered FPS.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct

spec=importlib.util.spec_from_file_location('setfile_import',Path(__file__).with_name('import-2ld-setfile.py'))
imp=importlib.util.module_from_spec(spec);spec.loader.exec_module(imp)


def catalogue(path):
    data=path.read_bytes(); source=data.decode(); clean=re.sub(r'/\*.*?\*/|//[^\n]*','',source,flags=re.S)
    listing=re.search(r'static const u32 \*sensor_2ld_setfiles_A\[\]\s*=\s*\{(.*?)\};',clean,re.S)
    if listing is None:raise ValueError('missing ordered mode list')
    names=re.findall(r'sensor_2ld_setfile_A_\w+',listing[1]); modes=[]
    for index,name in enumerate(names):
        suffix=name.removeprefix('sensor_2ld_setfile_A_')
        shape=re.fullmatch(r'(\d+)x(\d+)_(\d+)fps(.*)',suffix)
        if shape is None:raise ValueError('unknown mode name '+name)
        w,h,fps=map(int,shape.groups()[:3]); special='ssm' in suffix.lower()
        body=re.search(re.escape(name)+r'\[\]\s*=\s*\{(.*?)\};',clean,re.S)
        if body is None:raise ValueError('missing mode '+name)
        values=imp.table(source,name) if body[1].strip() else []
        pll=re.search('sensor_2ld_pllinfo_A_'+re.escape(suffix)+r'\s*=\s*\{(.*?)\};',clean,re.S)
        if pll is None:raise ValueError('missing PLL '+suffix)
        fields=[f.strip() for f in pll[1].split(',') if f.strip()]
        timing=None
        if fields:
            if len(fields)!=5:raise ValueError('unknown PLL shape')
            rate,pclk,frame,line=(int(t,0) for t in fields[1:])
            timing=dict(mipi_rate=rate,pclk=pclk,frame_lines=frame,line_clocks=line,
                        line_ns_floor=line*10**9//pclk,fps_from_pll=pclk/(frame*line))
        regs={};page=None
        for reg,val,size in zip(values[::3],values[1::3],values[2::3]):
            if reg==0xfcfc:page=val
            elif page==0x4000 and reg<0x10000:regs[reg]=val
        selected={f'0x{r:04x}':regs[r] for r in (0x0340,0x0342,0x034c,0x034e,0x0900,0x021e,0x0b30,0x0b80,0x0114) if r in regs}
        if values and (regs.get(0x034c),regs.get(0x034e))!=(w,h):raise ValueError('table dimensions differ from name '+name)
        if timing and (regs.get(0x0340),regs.get(0x0342))!=(timing['frame_lines'],timing['line_clocks']):raise ValueError('PLL/register mismatch '+name)
        modes.append(dict(stock_index=index,name=suffix,width=w,height=h,label_fps=fps,
                          special_ssm=special,empty_table=not values,triplets=len(values)//3,
                          table_sha256=hashlib.sha256(struct.pack('<'+'I'*len(values),*values)).hexdigest(),
                          timing=timing,registers=selected,
                          raw10_MB_s_at_label_fps=round(w*h*1.25*fps/1e6,3),
                          ios_status='not enabled by this catalogue'))
    return dict(source_sha256=hashlib.sha256(data).hexdigest(),modes=modes,
                boundary='Source inventory only; SSM and PLL FPS do not establish working capture/video modes.')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('setfile',type=Path);p.add_argument('--output',type=Path)
    a=p.parse_args()
    try:r=catalogue(a.setfile)
    except (ValueError,OSError) as e:p.error(str(e))
    text=json.dumps(r,indent=2)+'\n'
    if a.output:a.output.write_text(text)
    else:print(text,end='')
