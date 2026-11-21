#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define FP_TEE_MAX_CPUS 8u
static char out_buf[8192];
static bool fp_channels_attempted,fp_channels_service,irq_ok=true;
static uint64_t fp_channels_result,smc_result;
static unsigned fp_channels_registered,calls,irq_prepares,requests,panics;
static uint32_t response[4]={4,8,0xf0,1};
static unsigned fp_channel_cpu_count(void){return 8;}
static bool fp_sysconf_prepare_irqs(void){irq_prepares++;return irq_ok;}
static void fp_sysconf_prepare_request(void){requests++;}
static unsigned fp_sysconf_event_count(void){return 0;}
static unsigned fp_sysconf_panic_count(void){return panics;}
static uint64_t fp_channel_call(unsigned cpu,uint64_t f,uint64_t a,uint64_t b){
    assert(!cpu && f==0xb2000009 && !a && !b);calls++;return smc_result;
}
static void fp_sysconf_read_words(uint32_t out[4]){memcpy(out,response,16);}
#include "../../xnu/files/bsd/sys/redwood_fp_sysconf.h"
static void reset(void){
    fp_sysconf_attempted=fp_sysconf_valid=false;fp_sysconf_result=0;
    memset(fp_sysconf_words,0,16);calls=irq_prepares=requests=panics=0;
    irq_ok=true;smc_result=0;response[1]=8;
}
int main(void){
    fp_tee_sysconf(0);fp_tee_sysconf(2);fp_tee_sysconf(1);
    assert(!calls && !irq_prepares && !requests);
    fp_channels_attempted=fp_channels_service=true;fp_channels_registered=7;
    fp_tee_sysconf(1);assert(!calls && !irq_prepares);
    fp_channels_registered=8;fp_tee_sysconf(1);
    assert(calls==1 && requests==1 && irq_prepares==1 && fp_sysconf_valid);
    fp_tee_sysconf(1);fp_tee_sysconf(0);assert(calls==1);
    panics=1;fp_tee_sysconf(0);assert(!fp_sysconf_valid);
    reset();irq_ok=false;fp_tee_sysconf(1);assert(!calls && !requests);
    reset();smc_result=0xffffffff;fp_tee_sysconf(1);assert(calls==1 && !fp_sysconf_valid);
    reset();response[1]=255;fp_tee_sysconf(1);assert(!fp_sysconf_valid);
    reset();response[1]=0;fp_tee_sysconf(1);assert(!fp_sysconf_valid);
    puts("PASS: no implicit IRQ/SMC, complete-channel prerequisite, single exchange, IRQ errors, firmware errors, invalid response and panic rejection");
}
