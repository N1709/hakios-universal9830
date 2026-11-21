#!/usr/bin/env python3
"""Execute the actual hub pump with a fake clock and GPIO transport."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
s = (root / "xnu/files/bsd/dev/redwood_ssp.c").read_text()
a = s.index("static void\npump(")
b = s.index("\n/* ---- the transport", a)
body = s[a:b]
harness = r'''#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define NSEC_PER_USEC 1000
static uint32_t idle_us=4000,gnss_poll_users;
static int redwood_display_is_on;
static uint64_t clock_us; static unsigned sleeps,largest;
static uint64_t deadline_ms(uint32_t ms){return clock_us+ms*1000;}
static uint64_t mach_absolute_time(void){return clock_us;}
static bool host_req(void){return false;}
static bool exchange(const void *p,unsigned n){(void)p;(void)n;return true;}
static void delay_for_interval(unsigned us,unsigned unit){assert(unit==1000);clock_us+=us;sleeps++;if(us>largest)largest=us;}
'''
harness += "\nstatic void delay(unsigned us){delay_for_interval(us,1000); }\n"
harness += body
harness += r'''
static bool finished(void){return true;}
static void run(unsigned users,int display,unsigned expected){
 clock_us=0;sleeps=largest=0;gnss_poll_users=users;redwood_display_is_on=display;
 pump(100,NULL);assert(clock_us==100000);assert(largest==expected);assert(sleeps==100000/expected);
}
int main(void){run(0,0,100000);run(1,0,4000);run(2,0,4000);run(0,1,4000);run(0,0,100000);
 clock_us=sleeps=0;pump(100,finished);assert(!clock_us&&!sleeps);return 0;}
'''
with tempfile.TemporaryDirectory() as td:
 c=Path(td)/"pump.c"; exe=Path(td)/"pump"; c.write_text(harness)
 subprocess.run(["clang","-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",str(c),"-o",str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
print("hub polling: idle, active clients, display and early completion passed")
