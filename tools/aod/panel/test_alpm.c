#include "alpm.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
struct packet {uint8_t bytes[8],offset;size_t length;};
struct mock {struct packet packets[80];unsigned count,fail_at,waits[8],wait_count,restores;bool restore_ok;};
static bool send_packet(void *ctx,const uint8_t *p,size_t n,uint8_t offset){
    struct mock *m=ctx;assert(m->count<80&&n<=8);
    struct packet *r=&m->packets[m->count++];memcpy(r->bytes,p,n);r->length=n;r->offset=offset;
    return m->count!=m->fail_at;
}
static void wait_us(void *ctx,unsigned n){struct mock *m=ctx;assert(m->wait_count<8);m->waits[m->wait_count++]=n;}
static bool restore(void *ctx){struct mock *m=ctx;m->restores++;return m->restore_ok;}
static struct rw_alpm_gate gate(void){
    struct rw_alpm_gate g={true,true,true,true,true,{0x81,0x01,0x03},100000};return g;
}
static struct rw_alpm adapter(struct mock *m){struct rw_alpm a={RW_ALPM_NORMAL,m,send_packet,wait_us,restore};return a;}
static void packet(struct mock *m,unsigned i,const uint8_t *p,size_t n,uint8_t offset){
    assert(m->packets[i].length==n&&m->packets[i].offset==offset&&!memcmp(m->packets[i].bytes,p,n));
}
int main(void){
    struct rw_alpm_gate g=gate();struct mock m={.restore_ok=true};struct rw_alpm a=adapter(&m);
    assert(rw_alpm_enter(&a,&g)&&a.state==RW_ALPM_ACTIVE);
    assert(m.count==14&&m.wait_count==2&&m.waits[0]==24000&&m.waits[1]==16700);
    const uint8_t nit[]={0xbb,0x0b,0x0c,0x0c,0x43,0x0c,0x43,0x80};packet(&m,5,nit,sizeof(nit),0);
    const uint8_t isc[]={0xf6,0xff},avs[]={0xfd,0x0c},avc_off[]={0xf4,0x8a};
    packet(&m,7,isc,2,0x10);packet(&m,8,avs,2,0x28);packet(&m,11,avc_off,2,0x09);
    assert(rw_alpm_enter(&a,&g)&&m.count==14); /* idempotence */
    assert(rw_alpm_exit(&a)&&a.state==RW_ALPM_NORMAL&&m.restores==1&&m.waits[2]==34000);
    const uint8_t aswire[]={0xb5,0x00},avc_on[]={0xf4,0xca},key_off[]={0xf0,0xa5,0xa5};
    packet(&m,16,aswire,2,0x53);packet(&m,17,avc_on,2,0x09);packet(&m,20,key_off,3,0);
    assert(rw_alpm_exit(&a)&&m.count==21);
    /* Wrong panel / unsafe mode / incomplete restoration preparation must emit zero writes. */
    for(unsigned i=0;i<8;i++){
        m=(struct mock){.restore_ok=true};a=adapter(&m);g=gate();
        switch(i){case 0:g.experimental_enabled=false;break;case 1:g.display_on=false;break;
        case 2:g.normal_mode_60hz=false;break;case 3:g.no_pending_transition=false;break;
        case 4:g.normal_settings_saved=false;break;default:g.panel_id[i-5]^=1;break;}
        assert(!rw_alpm_enter(&a,&g)&&!m.count&&a.state==RW_ALPM_NORMAL);
    }
    /* Every individual failed entry transfer must attempt full exit and normal restoration. */
    for(unsigned failure=1;failure<=14;failure++){
        m=(struct mock){.restore_ok=true,.fail_at=failure};a=adapter(&m);g=gate();
        assert(!rw_alpm_enter(&a,&g));assert(m.restores==1&&a.state==RW_ALPM_NORMAL);
        packet(&m,m.count-1,key_off,3,0);
    }
    /* Exit failures must not report a normal panel; re-entry stays blocked until recovered. */
    for(unsigned failure=1;failure<=7;failure++){
        m=(struct mock){.restore_ok=true,.fail_at=failure};a=adapter(&m);a.state=RW_ALPM_ACTIVE;
        assert(!rw_alpm_exit(&a)&&a.state==RW_ALPM_FAULT&&m.count==7&&m.restores==1);
        unsigned writes=m.count;g=gate();assert(!rw_alpm_enter(&a,&g)&&m.count==writes);
        m.fail_at=0;assert(rw_alpm_exit(&a)&&a.state==RW_ALPM_NORMAL);
    }
    m=(struct mock){.restore_ok=false};a=adapter(&m);a.state=RW_ALPM_ACTIVE;
    assert(!rw_alpm_exit(&a)&&a.state==RW_ALPM_FAULT);
    puts("PASS: ALPM profile gates, ordering/offsets/timing, idempotence, every transfer failure, restoration and fault recovery");
}
