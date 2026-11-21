#!/usr/bin/env python3
"""Run the real framebuffer power/swap dispatch cases against a recording owner."""
from pathlib import Path
import re
import subprocess
import tempfile
repo=Path(__file__).resolve().parents[2]
s=(repo/'xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp').read_text()
start=s.index('\tcase kFBMethodSwapSignal:',s.index('IOMobileFramebufferUserClient::externalMethod'))
end=s.index('\tcase kFBMethodChangeFrameInfo:',start)
cases=s[start:end]
names=['kFBMethodSwapSignal','kFBMethodSwapCancel','kFBMethodSwapCancelAll','kFBMethodRequestPowerChange']
values={name:int(re.search(r'\b'+name+r'\s*=\s*(\d+)',s).group(1)) for name in names}
fixture='''
#include <cassert>
#include <cstdint>
#include <cstdio>
constexpr int kIOReturnSuccess=0;
struct Args{unsigned scalarInputCount;};
struct Owner{unsigned calls=0;bool on=false;void displayPower(bool v){calls++;on=v;}};
'''+''.join(f'constexpr unsigned {n}={v};\n' for n,v in values.items())+'''
int dispatch(Owner *fOwner,unsigned selector,Args *args,uint64_t *in){switch(selector){
'''+cases+'''
default:return -1;}}
int main(){
    Owner f;Args args{1};uint64_t input[1];
    for(unsigned method:{20u,52u,81u})for(uint64_t value:{0ull,1ull,12345ull}){
        input[0]=value;assert(dispatch(&f,method,&args,input)==0);assert(f.calls==0);
    }
    input[0]=0;assert(dispatch(&f,12,&args,input)==0&&f.calls==1&&!f.on);
    input[0]=1;assert(dispatch(&f,12,&args,input)==0&&f.calls==2&&f.on);
    args.scalarInputCount=0;assert(dispatch(&f,12,&args,input)==0&&f.calls==2);
    puts("PASS: actual swap dispatch never changes panel power; explicit on/off still works");
}
'''
fixture=fixture.replace('#include <cstdio>','#include <cstdio>\n#include <initializer_list>')
with tempfile.TemporaryDirectory(prefix='display-dispatch-') as d:
    p=Path(d);(p/'test.cpp').write_text(fixture)
    subprocess.run(['xcrun','clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
