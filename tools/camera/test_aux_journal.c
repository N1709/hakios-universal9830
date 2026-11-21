#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/bsd/dev/redwood_camera_s20_aux_journal.h"
struct mock{uint32_t regs[4][256];unsigned calls,fail,writes;bool corrupt;};
static bool rd(void *p,unsigned r,unsigned o,uint32_t *v){struct mock*m=p;assert(r<4&&o<256);*v=m->regs[r][o];return ++m->calls!=m->fail;}
static bool wr(void*p,unsigned r,unsigned o,uint32_t v){struct mock*m=p;m->regs[r][o]=m->corrupt?v^1:v;m->writes++;return ++m->calls!=m->fail;}
static bool configure(struct ax_journal*j,struct ax_io io){return ax_jwrite(j,io,0,4,0xff,0x12)&&ax_jwrite(j,io,1,8,3,2)&&ax_jwrite(j,io,0,4,0xf00,0xa00)&&ax_jwrite(j,io,2,12,~0U,0x12345678);}
int main(void){struct mock m={0},before={0};m.regs[0][4]=0xdeadbeef;m.regs[1][8]=0x12345678;before=m;
 struct ax_journal j={0};struct ax_io io={&m,rd,wr};assert(configure(&j,io));unsigned count=m.calls;
 assert(j.count==3);assert(ax_jrestore(&j,io));assert(!memcmp(m.regs,before.regs,sizeof(m.regs)));
 for(unsigned i=1;i<=count;i++){m=before;m.fail=i;j=(struct ax_journal){0};assert(!configure(&j,io));assert(ax_jrestore(&j,io));assert(!memcmp(m.regs,before.regs,sizeof(m.regs)));}
 m=before;j=(struct ax_journal){0};assert(configure(&j,io));m.regs[1][8]^=0x10000;assert(ax_jrestore(&j,io));assert(m.regs[1][8]==(before.regs[1][8]^0x10000));
 m=before;j=(struct ax_journal){0};assert(configure(&j,io));m.fail=m.calls+1;assert(!ax_jrestore(&j,io));assert(j.count==0);
 m=before;j=(struct ax_journal){0};m.corrupt=true;assert(!ax_jwrite(&j,io,0,4,0xff,0x12));assert(j.count==1);
 puts("PASS: every configuration fault, latched write rollback, first snapshot/mask union, unrelated-bit preservation and cleanup failures");}
