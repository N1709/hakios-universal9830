#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/bsd/dev/redwood_camera_s20_gw2_power.h"
struct mock {uint8_t regs[4][256];unsigned calls,fail,writes,pauses;uint16_t sequence[32];};
static bool rd(void *p,uint8_t chip,uint8_t reg,uint8_t *v){struct mock*m=p;assert(chip==2||chip==3);*v=m->regs[chip][reg];return ++m->calls!=m->fail;}
static bool wr(void *p,uint8_t chip,uint8_t reg,uint8_t v){struct mock*m=p;bool allowed=false;
 for(unsigned i=0;i<8;i++)if(cam_gw2_rails[i].pmic==chip&&cam_gw2_rails[i].reg==reg)allowed=true;
 assert(allowed);m->regs[chip][reg]=v;m->sequence[m->writes++]=(uint16_t)((chip<<8)|reg);return ++m->calls!=m->fail;}
static void pause_us(void*p,unsigned us){struct mock*m=p;assert(us==100||us==2000||us==9);m->pauses+=us;}
static struct mock initial(void){struct mock m={0};m.regs[2][0x0b]=0xd8;m.regs[2][0x0c]=0x90;
 m.regs[2][0x0f]=0xd8;m.regs[2][0x10]=0x48;m.regs[2][0x2f]=0xec;
 for(unsigned i=0;i<8;i++){const struct cam_gw2_rail*r=&cam_gw2_rails[i];m.regs[r->pmic][r->reg]=r->pmic==3?0x40:0x30;}
 return m;}
int main(void){struct mock m=initial(),before=m;struct cam_gw2_power s={0};struct cam_gw2_io io={&m,rd,wr,pause_us};
 assert(cam_gw2_power_begin(&s,io));unsigned operations=m.calls;assert(m.pauses==2300);
 for(unsigned i=0;i<8;i++){const struct cam_gw2_rail*r=&cam_gw2_rails[i];assert(m.regs[r->pmic][r->reg]==(uint8_t)(r->value|(r->pmic==3?0x40:0)));}
 assert(cam_gw2_power_restore(&s,io));assert(m.pauses==2309);
 const uint16_t off_order[8]={0x22a,0x305,0x224,0x223,0x308,0x303,0x227,0x309};
 for(unsigned i=0;i<8;i++)assert(m.sequence[8+i]==off_order[i]);assert(!memcmp(m.regs,before.regs,sizeof(m.regs)));
 for(unsigned fault=1;fault<=operations;fault++){m=initial();m.fail=fault;s=(struct cam_gw2_power){0};
  assert(!cam_gw2_power_begin(&s,io));assert(cam_gw2_power_restore(&s,io));assert(!memcmp(m.regs,before.regs,sizeof(m.regs)));}
 for(unsigned i=0;i<8;i++){m=initial();const struct cam_gw2_rail*r=&cam_gw2_rails[i];m.regs[r->pmic][r->reg]=0x80;
  assert(!cam_gw2_power_begin(&s,io));assert(m.writes==0);}
 m=initial();m.regs[3][0x05]=0xac;before=m;assert(cam_gw2_power_begin(&s,io));
 assert(cam_gw2_power_restore(&s,io));assert(!memcmp(m.regs,before.regs,sizeof(m.regs)));
 m=initial();m.regs[2][0x0b]=0;assert(!cam_gw2_power_begin(&s,io));assert(m.writes==0);
 m=initial();assert(cam_gw2_power_begin(&s,io));m.fail=m.calls+1;assert(!cam_gw2_power_restore(&s,io));
 puts("PASS: GW2 two-PMIC fault rollback, reserved bits, shared/live rails, parent prerequisites and delays");}
