/* First-write MMIO journal. Masks exclude status/self-clearing command bits.
 * A failed/latched write is still recorded and rolled back. */
#ifndef REDWOOD_CAMERA_S20_AUX_JOURNAL_H
#define REDWOOD_CAMERA_S20_AUX_JOURNAL_H
#include <stdint.h>
#include <stdbool.h>
#define AX_JOURNAL_CAPACITY 192
struct ax_reg {unsigned region,offset;uint32_t before,mask;};
struct ax_journal {struct ax_reg regs[AX_JOURNAL_CAPACITY];unsigned count;};
struct ax_io {void *ctx;bool(*read)(void *,unsigned,unsigned,uint32_t *);bool(*write)(void *,unsigned,unsigned,uint32_t);};
static inline bool ax_jremember(struct ax_journal *j,struct ax_io io,unsigned region,unsigned off,uint32_t mask){
    for(unsigned i=0;i<j->count;i++)if(j->regs[i].region==region&&j->regs[i].offset==off){j->regs[i].mask|=mask;return true;}
    uint32_t before=0;
    if(!mask||j->count==AX_JOURNAL_CAPACITY||!io.read(io.ctx,region,off,&before))return false;
    j->regs[j->count++]=(struct ax_reg){region,off,before,mask};return true;
}
static inline bool ax_jwrite(struct ax_journal *j,struct ax_io io,unsigned region,unsigned off,uint32_t mask,uint32_t value){
    uint32_t before=0,after=0;unsigned i;
    if(!mask||!io.read(io.ctx,region,off,&before))return false;
    for(i=0;i<j->count;i++)if(j->regs[i].region==region&&j->regs[i].offset==off)break;
    if(i==j->count){
        if(j->count==AX_JOURNAL_CAPACITY)return false;
        j->regs[j->count++]=(struct ax_reg){region,off,before,mask};
    }else j->regs[i].mask|=mask;
    uint32_t want=(before&~mask)|(value&mask);
    return io.write(io.ctx,region,off,want)&&io.read(io.ctx,region,off,&after)&&!((after^want)&mask);
}
static inline bool ax_jrestore(struct ax_journal *j,struct ax_io io){
    bool ok=true;
    while(j->count){struct ax_reg *r=&j->regs[--j->count];uint32_t now=0,after=0;
        if(!io.read(io.ctx,r->region,r->offset,&now)){ok=false;continue;}
        if(!((now^r->before)&r->mask))continue;
        bool written=io.write(io.ctx,r->region,r->offset,(now&~r->mask)|(r->before&r->mask));
        bool read=io.read(io.ctx,r->region,r->offset,&after);
        if(!written||!read||((after^r->before)&r->mask))ok=false;
    }
    return ok;
}
#endif
