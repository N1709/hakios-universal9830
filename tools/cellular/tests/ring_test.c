#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../s5123-ring.h"
struct model {uint8_t memory[64];unsigned calls,fail,publishes;int after;};
static int load(void *ctx,uint32_t off,uint32_t *value)
{struct model *m=ctx;if (++m->calls==m->fail)return -1;memcpy(value,m->memory+off,4);return 0;}
static int put(void *ctx,uint32_t off,const uint8_t *p,size_t n)
{struct model *m=ctx;assert(off>=16 && off+n<=32);int fail=++m->calls==m->fail;if(fail&&!m->after)return -1;memcpy(m->memory+off,p,n);return fail?-1:0;}
static int get(void *ctx,uint32_t off,uint8_t *p,size_t n)
{struct model *m=ctx;assert(off>=16 && off+n<=32);if(++m->calls==m->fail)return -1;memcpy(p,m->memory+off,n);return 0;}
static int publish(void *ctx,uint32_t off,uint32_t value)
{struct model *m=ctx;int fail=++m->calls==m->fail;if(fail&&!m->after)return -1;memcpy(m->memory+off,&value,4);m->publishes++;return fail?-1:0;}
static uint32_t word(struct model *m,unsigned off){uint32_t v;memcpy(&v,m->memory+off,4);return v;}
static void setup(struct model *m,uint32_t head,uint32_t tail)
{memset(m,0,sizeof(*m));memcpy(m->memory,&head,4);memcpy(m->memory+4,&tail,4);}
int main(void)
{
    const struct s5123_ring ring={16,16,0,4};struct model m;
    struct s5123_ring_io io={&m,load,put,get,publish};uint8_t payload[16],out[16];
    for(unsigned i=0;i<16;i++)payload[i]=(uint8_t)(i+1);
    unsigned cases=0;
    /* Exhaust all head/tail positions and nonempty lengths against an
     * independent byte-by-byte circular oracle, for both queue directions. */
    for(unsigned tx=0;tx<2;tx++)for(unsigned h=0;h<16;h++)for(unsigned t=0;t<16;t++)for(unsigned n=1;n<16;n++) {
        setup(&m,h,t);unsigned available=0,at=tx?h:t;
        while(available<15 && (tx?((at+1)%16!=t):(at!=h))) {available++;at=(at+1)%16;}
        for(unsigned i=0;i<16;i++)m.memory[16+i]=(uint8_t)i;
        int rc=s5123_ring_transfer(&io,&ring,64,tx?payload:out,n,(int)tx);
        if(n>available){assert(rc==S5123_RING_SPACE && !m.publishes);}
        else {assert(rc==0 && m.publishes==1);for(unsigned i=0;i<n;i++)
            assert(tx?m.memory[16+(h+i)%16]==payload[i]:out[i]==(t+i)%16);
            assert(word(&m,tx?0:4)==((tx?h:t)+n)%16);}
        cases++;
    }
    for(unsigned effect=0;effect<2;effect++)for(unsigned fail=1;fail<=5;fail++) {
        setup(&m,14,10);m.fail=fail;m.after=(int)effect;
        int rc=s5123_ring_transfer(&io,&ring,64,payload,4,1);
        if(fail<5)assert(rc==S5123_RING_IO && word(&m,0)==14 && !m.publishes);
        else assert(rc==S5123_RING_COMMIT_UNKNOWN);
    }
    setup(&m,16,0);assert(s5123_ring_transfer(&io,&ring,64,payload,1,1)==S5123_RING_INDEX);
    assert(s5123_ring_valid(&s5123_raw_tx,0x800000) && s5123_ring_valid(&s5123_raw_rx,0x800000));
    struct s5123_ring bad={UINT32_MAX-8,16,0,4};assert(!s5123_ring_valid(&bad,UINT32_MAX));
    bad=(struct s5123_ring){0,16,0,4};assert(!s5123_ring_valid(&bad,64));
    printf("IPC ring core: %u wrap/full/empty oracle cases, partial-copy and uncertain-publish faults, malformed indices/ranges passed\n",cases);
}
