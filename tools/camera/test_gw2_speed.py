"""Test the actual upload-speed wrapper, including cold replay and bus restoration."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[2]
s=(root/'xnu/files/bsd/dev/redwood_camera_s20.c').read_text();a=s.index('static bool cam_gw2_upload_slow;');b=s.index('static bool cam_s20_aux_run(',a)
prefix='''#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
struct redwood_bbi2c{uint32_t half_us;};
static int cam_s20_rear_i2c_half_us=1;static unsigned calls,failures,speeds[4];
static bool aux_gw2_program(struct redwood_bbi2c*b,bool full){(void)full;speeds[calls++]=b->half_us;return calls>failures;}
'''
suffix='''int main(void){struct redwood_bbi2c b={4};
assert(aux_gw2_tables(&b,true)&&calls==1&&speeds[0]==1&&b.half_us==4&&!cam_gw2_upload_slow);
calls=0;failures=1;assert(aux_gw2_tables(&b,true)&&calls==2&&speeds[0]==1&&speeds[1]==4&&b.half_us==4&&cam_gw2_upload_slow);
calls=0;failures=0;assert(aux_gw2_tables(&b,true)&&calls==1&&speeds[0]==4);
cam_gw2_upload_slow=false;calls=0;failures=2;assert(!aux_gw2_tables(&b,true)&&b.half_us==4&&calls==2);
puts("PASS: fast upload, whole-program slow replay, sticky fallback and bus restoration");}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(prefix+s[a:b]+suffix)
 subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True);subprocess.run([str(p/'test')],check=True)
