/* Exercise real rail plan under NACK-after-latched-write and readback faults. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/bsd/dev/redwood_camera_s20_aux_power.h"
struct mock {uint8_t regs[256];unsigned calls,fail,writes;};
static bool read_reg(void *p,uint8_t r,uint8_t *v){struct mock *m=p;*v=m->regs[r];return ++m->calls!=m->fail;}
static bool write_reg(void *p,uint8_t r,uint8_t v){
    struct mock *m=p;bool allowed=false;
    for(unsigned i=0;i<4;i++)if(r==cam_aux_power_regs[i])allowed=true;
    assert(allowed);m->regs[r]=v;m->writes++;return ++m->calls!=m->fail;
}
static struct mock initial(void){struct mock m={0};m.regs[0x0e]=0x90;m.regs[0x0d]=0x13;
    m.regs[0x29]=0x41;m.regs[0x20]=0x30;m.regs[0x25]=0x35;return m;}
int main(void){
    struct mock m=initial(),before=m;struct cam_aux_power s={0};
    struct cam_aux_power_io io={&m,read_reg,write_reg};
    assert(cam_aux_power_begin(&s,io));unsigned count=m.calls;
    assert(m.regs[0x0d]==0xd3&&m.regs[0x29]==0xd8&&m.regs[0x20]==0xa4&&m.regs[0x25]==0xb0);
    assert(cam_aux_power_restore(&s,io));assert(!memcmp(m.regs,before.regs,256));
    for(unsigned fault=1;fault<=count;fault++){
        m=initial();s=(struct cam_aux_power){0};m.fail=fault;
        assert(!cam_aux_power_begin(&s,io));assert(cam_aux_power_restore(&s,io));
        assert(!memcmp(m.regs,before.regs,256));
    }
    m=initial();m.regs[0x0e]=0x91;assert(!cam_aux_power_begin(&s,io));assert(m.writes==0);
    for(unsigned i=1;i<4;i++){
        m=initial();m.regs[cam_aux_power_regs[i]]|=0x80;
        assert(!cam_aux_power_begin(&s,io));assert(m.writes==0);
    }
    /* A cleanup error is surfaced, and cleanup continues for all other rails. */
    m=initial();assert(cam_aux_power_begin(&s,io));m.fail=m.calls+1;
    assert(!cam_aux_power_restore(&s,io));assert(!memcmp(m.regs,before.regs,256));
    puts("PASS: all transaction faults, latched NACK rollback, voltage/live-rail guards, cleanup error reporting");
}
