/* MIT. Power lifecycle model; no physical GPIO, firmware, SIM or modem access. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../s5123-boot.h"
#include "../s5123-gpio-fields.h"

struct model {
    int output[S5123_OUTPUT_COUNT], guard, isolated;
    int prepared, routed, parked, restored, sent, short_transfer, reject_shutdown;
    unsigned calls, fail_at, hit;
    uint64_t elapsed, powered_at, released_at;
};
static int fault(struct model *m)
{
    m->calls++;
    if (m->calls==m->fail_at) {m->hit++;return -1;}
    return 0;
}
static int ready(void *p) {struct model *m=p;return m->guard && m->isolated;}
static int prepare(void *p) {struct model *m=p;m->prepared=1;return fault(m);}
static int output(void *p, enum s5123_ap_output signal, int value)
{
    struct model *m=p;
    assert(signal>=0 && signal<S5123_OUTPUT_COUNT && (value==0 || value==1));
    if (m->reject_shutdown && m->sent && !value && (signal==S5123_RESET_N || signal==S5123_POWER_ON)) return -1;
    m->output[signal]=value; /* Fault can occur AFTER a real side effect. */
    if (signal==S5123_POWER_ON && value) {
        assert(!m->output[S5123_RESET_N] && m->elapsed>=50050);
        m->powered_at=m->elapsed;
    }
    if (signal==S5123_RESET_N && value) {
        assert(m->output[S5123_POWER_ON] && m->elapsed-m->powered_at>=50000);
        m->released_at=m->elapsed;
    }
    return fault(m);
}
static int sleep_us(void *p,uint32_t delay)
{
    struct model *m=p;assert(delay==50 || delay==50000);
    /* A failed timer may return without any time passing. */
    int error=fault(m);if (!error) m->elapsed+=delay;return error;
}
static int route(void *p)
{
    struct model *m=p;
    assert(m->output[S5123_RESET_N] && m->output[S5123_POWER_ON]);
    assert(m->output[S5123_AP_STATUS] && !m->output[S5123_AP_WAKE] && !m->output[S5123_DUMP_NOTI]);
    assert(m->elapsed-m->released_at>=50000);
    m->routed=1;return fault(m);
}
static int send(void *p,const uint8_t *image,size_t length,uint32_t *completed)
{
    struct model *m=p;assert(m->prepared && m->routed && image && length);
    m->sent=1;m->parked=0;m->elapsed+=1000;
    *completed=(uint32_t)(m->short_transfer ? length-1 : length);
    return fault(m);
}
static int stop(void *p)
{
    struct model *m=p;int error=fault(m);m->parked=!error;return error;
}
static int restore(void *p)
{
    struct model *m=p;int error=fault(m);
    if (!error) {m->restored=1;m->prepared=0;m->routed=0;}
    return error;
}
static int quiescent(void *p)
{
    struct model *m=p;
    return !fault(m) && !m->output[S5123_RESET_N] && !m->output[S5123_POWER_ON] && m->isolated;
}
static struct s5123_boot_ops operations(struct model *m)
{
    return (struct s5123_boot_ops){m,ready,prepare,output,sleep_us,route,send,stop,restore,quiescent};
}
static unsigned run(unsigned fail_at)
{
    const uint8_t image[]="synthetic BOOT bytes; not firmware";
    struct model m={.guard=1,.isolated=1,.fail_at=fail_at};
    struct s5123_boot_ops ops=operations(&m);
    struct s5123_boot_result result={0};
    int error=s5123_boot_once(&ops,&result,image,sizeof(image),s5123_boot_crc32(image,sizeof(image)));
    if (!fail_at) {
        assert(error==0 && result.phase==S5123_BOOT_TX_COMPLETE);
        assert(result.spi_completed==sizeof(image) && result.cleanup_safe && !result.cleanup_error);
        assert(m.elapsed>=200050 && m.parked && m.restored && !m.prepared && !m.routed);
    } else {
        assert(m.hit==1 && error!=0);
    }
    if (result.cleanup_safe) {
        assert(!result.cleanup_error);
        assert(!m.output[S5123_RESET_N] && !m.output[S5123_POWER_ON]);
        assert(m.restored && !m.prepared && !m.routed);
        if (result.power_requested) assert(m.parked && m.isolated);
    }
    assert(result.attempted);
    unsigned calls=m.calls;
    assert(s5123_boot_once(&ops,&result,image,sizeof(image),s5123_boot_crc32(image,sizeof(image)))==S5123_BOOT_ALREADY_ATTEMPTED);
    assert(m.calls==calls);
    return calls;
}
int main(void)
{
    for (unsigned pin=0;pin<8;pin++) for (unsigned value=0;value<2;value++) {
        uint32_t con,dat,original_con=0xa5a5a5a5,original_dat=0x5a;
        assert(s5123_gpio_output_values(original_con,original_dat,pin,value,&con,&dat));
        assert(((con>>(4*pin))&15)==1 && ((dat>>pin)&1)==value);
        assert((con&~(15u<<(4*pin)))==(original_con&~(15u<<(4*pin))));
        assert((dat&~(1u<<pin))==(original_dat&~(1u<<pin)));
    }
    uint32_t con,dat;
    assert(!s5123_gpio_output_values(0,0,8,0,&con,&dat));
    assert(!s5123_gpio_output_values(0,0,3,2,&con,&dat));
    assert(s5123_boot_crc32((const uint8_t *)"123456789",9)==0xcbf43926);
    unsigned stages=run(0);
    for (unsigned i=1;i<=stages;i++)run(i);
    const uint8_t image[]={1,2,3,4};
    struct model m={.guard=1,.isolated=1};
    struct s5123_boot_ops ops=operations(&m);
    struct s5123_boot_result result={0};
    uint32_t crc=s5123_boot_crc32(image,sizeof(image));
    assert(s5123_boot_once(&ops,&result,image,sizeof(image),crc^1)==S5123_BOOT_BAD_IMAGE);
    assert(!m.calls && !result.attempted);
    m.isolated=0;
    assert(s5123_boot_once(&ops,&result,image,sizeof(image),crc)==S5123_BOOT_GUARD);
    assert(!m.calls && !result.attempted);
    m.isolated=1;m.short_transfer=1;
    assert(s5123_boot_once(&ops,&result,image,sizeof(image),crc)==S5123_BOOT_SPI);
    assert(result.cleanup_safe && result.spi_completed==sizeof(image)-1);
    m=(struct model){.guard=1,.isolated=1,.reject_shutdown=1};
    ops=operations(&m);result=(struct s5123_boot_result){0};
    assert(s5123_boot_once(&ops,&result,image,sizeof(image),crc)==S5123_BOOT_CLEANUP);
    assert(!result.cleanup_safe && m.output[S5123_RESET_N] && m.output[S5123_POWER_ON]);
    printf("CP BOOT lifecycle: success + %u injected callback failures, CRC/PHY guards, short transfer and unsafe-shutdown hold passed\n",stages);
}
