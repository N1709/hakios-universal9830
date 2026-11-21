#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "../../xnu/files/bsd/dev/redwood_camera_s20_main_voltage.h"
struct model{uint8_t r[4][256];unsigned ops,fail,writes;bool latch;};
static bool rd(void *ctx,uint8_t p,uint8_t r,uint8_t *v){struct model*m=ctx;if(++m->ops==m->fail)return false;*v=m->r[p][r];return true;}
static bool wr(void *ctx,uint8_t p,uint8_t r,uint8_t v){struct model*m=ctx;m->writes++;bool ok=++m->ops!=m->fail;if(ok||m->latch)m->r[p][r]=v;return ok;}
static struct model initial(void){struct model m={0};m.r[2][0x1e]=0x18;m.r[3][4]=0x40;m.r[3][5]=0x2c;m.r[3][7]=0x60;return m;}
static bool rd03(void *ctx,uint8_t reg,uint8_t *value){return rd(ctx,3,reg,value);}
static bool wr03(void *ctx,uint8_t reg,uint8_t value){return wr(ctx,3,reg,value);}
int main(void){
 for(unsigned fault=1;fault<=9;fault++)for(unsigned latch=0;latch<2;latch++){
  struct model m=initial(),before=m;struct cam_main_voltage s={0};m.fail=fault;m.latch=latch;
  struct cam_main_voltage_io io={&m,rd,wr};assert(!cam_main_voltage_begin(&s,io));
  assert(cam_main_voltage_restore(&s,io));assert(!memcmp(m.r,before.r,sizeof(m.r))&&!s.changed);
 }
 struct model m=initial(),before=m;struct cam_main_voltage s={0};struct cam_main_voltage_io io={&m,rd,wr};
 assert(cam_main_voltage_begin(&s,io));assert(m.r[2][0x1e]==0x24&&m.r[3][4]==0x5c&&m.r[3][7]==0x6e);
 assert(cam_main_voltage_restore(&s,io)&&!memcmp(m.r,before.r,sizeof(m.r)));
 m=initial();s=(struct cam_main_voltage){0};m.r[2][0x1e]|=0x80;assert(!cam_main_voltage_begin(&s,io)&&!m.writes);
 m=initial();s=(struct cam_main_voltage){0};assert(cam_main_voltage_begin(&s,io));m.r[2][0x1e]|=0x80;
 assert(!cam_main_voltage_restore(&s,io));assert(m.r[2][0x1e]==0xa4);m.r[2][0x1e]&=0x7f;assert(cam_main_voltage_restore(&s,io));
 m=initial();s=(struct cam_main_voltage){0};assert(cam_main_voltage_begin(&s,io));m.r[3][7]=0x2f;
 assert(!cam_main_voltage_restore(&s,io)&&m.r[3][7]==0x2f);
 struct model enable=initial();struct cam_main_enable_io eio={&enable,rd03,wr03};
 enable.r[3][4]=0x5c;assert(cam_main_enable_bits(eio,4,0x80)&&enable.r[3][4]==0xdc);
 assert(cam_main_enable_bits(eio,4,0)&&enable.r[3][4]==0x5c); /* MUST really turn off. */
 enable.r[3][7]=0x6e;assert(cam_main_enable_bits(eio,7,0x80)&&enable.r[3][7]==0xee);
 assert(cam_main_enable_bits(eio,7,0)&&enable.r[3][7]==0x6e);
 assert(!cam_main_enable_bits(eio,8,0x80));
 for(unsigned fault=0;fault<=9;fault++)for(unsigned latch=0;latch<2;latch++){
  struct model cycle=initial(),original=cycle;struct cam_main_voltage transaction={0};
  struct cam_main_voltage_io vio={&cycle,rd,wr};struct cam_main_enable_io en={&cycle,rd03,wr03};
  assert(cam_main_voltage_begin(&transaction,vio));
  cycle.fail=fault?cycle.ops+fault:0;cycle.latch=latch;
  const uint8_t regs[]={5,7,4};bool enabled=true;
  for(unsigned i=0;i<3;i++)if(!cam_main_enable_bits(en,regs[i],0x80)){enabled=false;break;}
  if(!fault)assert(enabled&&cycle.r[3][4]==0xdc&&cycle.r[3][7]==0xee);
  cycle.fail=0;
  for(unsigned i=0;i<3;i++)assert(cam_main_enable_bits(en,regs[i],original.r[3][regs[i]]&0x80));
  assert(cam_main_voltage_restore(&transaction,vio));
  assert(!memcmp(cycle.r,original.r,sizeof(cycle.r)));
 }
 puts("PASS: primary selectors, all startup faults/latched NACKs, reserved bits, live-rail refusal and rollback ownership");
}
