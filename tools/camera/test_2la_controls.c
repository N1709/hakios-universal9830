#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/bsd/dev/redwood_camera_s20_2la_controls.h"
struct mock{uint16_t regs[65536];unsigned calls,fail,writes;};
static bool rd(void*p,uint16_t reg,uint16_t*v){struct mock*m=p;*v=m->regs[reg];return ++m->calls!=m->fail;}
static bool wr(void*p,uint16_t reg,uint16_t v){struct mock*m=p;m->regs[reg]=v;m->writes++;return ++m->calls!=m->fail;}
static bool wr8(void*p,uint16_t reg,uint8_t v){assert((reg==0x0702||reg==0x0704)&&v==0);return wr(p,reg,v);}
static struct mock initial(void){struct mock m={0};m.regs[0x0340]=6652;m.regs[0x0342]=11200;m.regs[0x0202]=16;m.regs[0x0204]=32;m.regs[0x020e]=256;return m;}
int main(void){struct mock m=initial();struct ax_2la_control_report r={0};struct ax_2la_control_io io={&m,rd,wr,wr8};
 assert(ax_2la_controls_apply(io,3000,0x80,&r));assert(r.applied&&r.before[0]==16&&r.after[0]==3000&&r.after[1]==0x80&&r.after[2]==0x100);
 unsigned count=m.calls;for(unsigned i=1;i<=count;i++){m=initial();m.fail=i;r=(struct ax_2la_control_report){0};assert(!ax_2la_controls_apply(io,3000,0x80,&r));assert(!r.applied&&m.calls==i);}
 m=initial();assert(!ax_2la_controls_apply(io,15,0x20,&r)&&m.calls==0);
 assert(!ax_2la_controls_valid(6637,0x20));assert(!ax_2la_controls_valid(100,0x201));assert(!ax_2la_controls_valid(UINT64_MAX,0x20));
 assert(ax_2la_controls_valid(16,0x20)&&ax_2la_controls_valid(6636,0x200));
 m=initial();m.regs[0x0340]=1000;assert(!ax_2la_controls_apply(io,3000,0x20,&r)&&m.writes==0);
 m=initial();m.regs[0x0340]=3608;m.regs[0x0342]=17680;
 assert(ax_gw2_controls_apply(io,3000,0x80,&r)&&r.applied);
 count=m.calls;for(unsigned i=1;i<=count;i++){m=initial();m.regs[0x0340]=3608;m.regs[0x0342]=17680;m.fail=i;r=(struct ax_2la_control_report){0};assert(!ax_gw2_controls_apply(io,3000,0x80,&r)&&m.calls==i&&!r.applied);}
 m=initial();m.regs[0x0340]=3608;m.regs[0x0342]=17680;
 assert(!ax_gw2_controls_apply(io,3593,0x20,&r)&&m.calls==0);
 m=initial();m.regs[0x0340]=5166;m.regs[0x0342]=12304;
 assert(ax_gw2_full_controls_apply(io,5000,0x80,&r)&&r.applied);
 assert(!ax_gw2_full_controls_apply(io,5151,0x20,&r));
 puts("PASS: 2LA stock bounds/mode guard, exposure-gain readback, and fail-fast at every I2C operation");}
