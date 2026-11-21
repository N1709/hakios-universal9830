#!/usr/bin/env python3
"""Execute the actual framebuffer fade methods with deterministic wake interleavings."""
from pathlib import Path
import subprocess
import tempfile
repo=Path(__file__).resolve().parents[2]
source=(repo/'xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp').read_text()
def function(name):
    start=source.index('IOMobileFramebuffer::'+name+'(')
    brace=source.index('{',start);depth=1;end=brace+1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return 'void\n'+source[start:end]+'\n'
fixture=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#ifndef __unused
#define __unused __attribute__((unused))
#endif
using thread_call_param_t=void*;
using thread_call_t=void*;
struct Lock {bool held=false;};
static Lock ds,bl;
static void (*beforeLock)()=nullptr;
static uint64_t nowValue=100;
static unsigned callbacks;
static void IOLockLock(Lock *l){
    if(l==&ds&&beforeLock){auto f=beforeLock;beforeLock=nullptr;f();}
    assert(!l->held);l->held=true;
}
static void IOLockUnlock(Lock *l){assert(l->held);l->held=false;}
static uint64_t mach_absolute_time(){return nowValue;}
static void nanoseconds_to_absolutetime(uint64_t n,uint64_t *v){*v=n;}
static void clock_interval_to_deadline(unsigned n,unsigned,uint64_t *v){*v=nowValue+n;}
static void thread_call_enter(void*){callbacks++;}
static void thread_call_enter_delayed(void*,uint64_t){callbacks++;}
static void *thread_call_allocate_with_options(void(*)(void*,void*),void*,int,int){return (void*)1;}
static unsigned refreshHz(){return 120;}
static constexpr unsigned NSEC_PER_SEC=1000000000,NSEC_PER_MSEC=1000000,kMicrosecondScale=1;
static constexpr int THREAD_CALL_PRIORITY_KERNEL=0,THREAD_CALL_OPTIONS_ONCE=0;
static bool gFade=true,gOffAlpha=true;
static unsigned gOffFadeMs=0;
class IOMobileFramebuffer {
public:
    Lock *fDsimLock=&ds,*fBLLock=&bl;
    void *fDsim=(void*)1,*fDecon=(void*)1;
    int fWin=0;
    unsigned fBLWrites=1,fBLCurrent=100,fFadeFrom=0,fBLTarget=100;
    uint64_t fFadeStart=0,fFadeLen=0,fAlphaStart=0,fAlphaLen=10;
    bool fDisplayOff=false,fAlphaUp=false,fAlphaOn=true;
    void *fBLCall=(void*)1,*fAlphaCall=(void*)1;
    unsigned painted=999,restored=0;
    static void alphaWork(void*,void*);
    void brightnessRequest(uint32_t,uint32_t);
    void alphaSet(unsigned n){assert(ds.held);painted=n;}
    void alphaRestore(){assert(ds.held);restored++;fAlphaOn=false;}
};
static IOMobileFramebuffer *subject;
static void wakeBeforeLock(){subject->fAlphaUp=true;subject->fAlphaStart=50;subject->fAlphaLen=100;}
'''
main=r'''
int main(){
    IOMobileFramebuffer f;subject=&f;
    // Callback queued for a completed fade-out; wake wins just before the lock.
    beforeLock=wakeBeforeLock;IOMobileFramebuffer::alphaWork(&f,nullptr);
    assert(f.painted==127&&f.fAlphaUp&&callbacks==1&&!ds.held);
    // Another brightness request cancels fade-out and restores registers under DSIM lock.
    f.fAlphaUp=false;f.fAlphaOn=true;f.brightnessRequest(50,0);
    assert(f.restored==1&&f.fAlphaLen==0&&f.fBLTarget==50&&!ds.held&&!bl.held);
    // An old callback after cancellation performs no register write or reschedule.
    unsigned count=callbacks;f.painted=999;IOMobileFramebuffer::alphaWork(&f,nullptr);
    assert(f.painted==999&&callbacks==count);
    // Fade-out reaches black once; it is retained for offWork, without polling forever.
    f.fAlphaLen=10;f.fAlphaStart=0;f.fAlphaUp=false;IOMobileFramebuffer::alphaWork(&f,nullptr);
    assert(f.painted==0&&f.fAlphaLen==10&&callbacks==count);
    // Completed fade-in restores the original window registers exactly once.
    f.fAlphaUp=true;f.fAlphaOn=true;IOMobileFramebuffer::alphaWork(&f,nullptr);
    assert(f.restored==2&&!f.fAlphaUp&&!f.fAlphaLen);
    puts("PASS: actual framebuffer methods survive stale fade/wake, cancellation and completion interleavings");
}
'''
with tempfile.TemporaryDirectory(prefix='display-races-') as d:
    p=Path(d);(p/'test.cpp').write_text(fixture+function('brightnessRequest')+function('alphaWork')+main)
    subprocess.run(['xcrun','clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
