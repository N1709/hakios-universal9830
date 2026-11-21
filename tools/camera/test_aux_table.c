#include <assert.h>
#include <stdio.h>
#include "../../xnu/files/bsd/dev/redwood_camera_s20_aux_table.h"
#include "../../xnu/files/bsd/dev/redwood_camera_s20_2la.h"
struct mock {unsigned writes,fail,delays;};
static bool write_reg(void*p,uint16_t r,uint16_t v){(void)r;(void)v;struct mock*m=p;return ++m->writes!=m->fail;}
static void pause_us(void*p,unsigned n){((struct mock*)p)->delays+=n;}
int main(void){struct mock m={0};struct cam_aux_table_io io={&m,write_reg,pause_us};
 const uint32_t bad[]={0x0100,0x100,2,0xf0000001,0x6f12,2};
 assert(!cam_aux_apply_table(io,bad,6));assert(m.writes==0);
 const uint32_t delay[]={0xf0000003,10001,0};assert(!cam_aux_apply_table(io,delay,3));
#define APPLY(t) cam_aux_apply_table(io,t,sizeof(t)/sizeof((t)[0]))
 assert(APPLY(redwood_2la_setfile_A_initial_setting));assert(m.delays==10000);
 assert(APPLY(redwood_2la_setfile_A_Global_2));assert(APPLY(redwood_2la_setfile_A_4032x3024_30fps));
 assert(APPLY(redwood_2la_dual_single_A_settings));
 unsigned total=m.writes;
 for(unsigned i=1;i<=total;i++){
  m=(struct mock){.fail=i};
  bool ok=APPLY(redwood_2la_setfile_A_initial_setting)&&APPLY(redwood_2la_setfile_A_Global_2)&&
          APPLY(redwood_2la_setfile_A_4032x3024_30fps)&&APPLY(redwood_2la_dual_single_A_settings);
  assert(!ok&&m.writes==i);
 }
 puts("PASS: A202 tables prevalidated, no burst firmware, exact delay, fail-fast at every I2C write");}
