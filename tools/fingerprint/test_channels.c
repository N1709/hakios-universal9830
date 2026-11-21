#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static char out_buf[8192];
static bool fp_tee_probe_attempted;
static uint64_t fp_tee_probe_result;
static unsigned allocations,calls,online_count=8,configured_cpus=8;
static int fail_at=-1,fail_alloc_at=-1;
static unsigned fp_channel_cpu_count(void) {return configured_cpus;}
static bool fp_channel_online(unsigned cpu) {return cpu<online_count;}
static bool fp_channel_allocate(unsigned i,uint32_t *pfn) {*pfn=0x10000+i*4;allocations++;return (int)i!=fail_alloc_at;}
static uint64_t fp_channel_call(unsigned cpu,uint64_t function,uint64_t arg1,uint64_t arg2) {
    unsigned call=calls++;
    if(call<8){assert(cpu==call && function==0xb2000000 && arg1==0x10000+call*4 && !arg2);}
    else {assert(call==8 && !cpu && function==0xb2000001 && arg1==1 && arg2==1);}
    return (int)call==fail_at?0xffffffffULL:0;
}
static void fp_channel_service_prepare(uint32_t pfn) {assert(pfn==0x10020);}
static void fp_channel_service_read(uint32_t *cpu,uint32_t *user) {*cpu=0xff;*user=0x3f;}
#include "../../xnu/files/bsd/sys/redwood_fp_channels.h"
static void reset(void) {
    fp_channels_attempted=false;fp_channels_registered=0;fp_channels_result=0;
    fp_channels_service=false;fp_channels_cpu_mask=fp_channels_user_mask=0;
    allocations=calls=0;
}
int main(void) {
    fp_tee_channels(0);assert(!allocations && !calls && strstr(out_buf,"NOT RUN"));
    fp_tee_channels(2);assert(!allocations && !calls);
    fp_tee_channels(1);assert(!allocations && !calls && strstr(out_buf,"REFUSED"));
    fp_tee_probe_attempted=true;fp_tee_probe_result=0xffffffff;
    fp_tee_channels(1);assert(!calls && !allocations);
    fp_tee_probe_result=0;online_count=7;fp_tee_channels(1);assert(!calls && !allocations);
    online_count=8,configured_cpus=8;fp_tee_channels(1);
    assert(allocations==9 && calls==9 && fp_channels_registered==8 && fp_channels_service);
    fp_tee_channels(1);fp_tee_channels(0);assert(calls==9 && allocations==9);
    reset();fail_at=3;fp_tee_channels(1);
    assert(calls==4 && fp_channels_registered==3 && !fp_channels_service);
    fp_tee_channels(1);assert(calls==4); /* never retry partial registration */
    reset();fail_at=8;fp_tee_channels(1);
    assert(calls==9 && fp_channels_registered==8 && !fp_channels_service);
    reset();fail_at=-1;fail_alloc_at=2;fp_tee_channels(1);
    assert(!calls && allocations==3);fp_tee_channels(1);assert(allocations==3);
    reset();fail_alloc_at=-1;configured_cpus=9;fp_tee_channels(1);assert(!calls && !allocations);
    puts("PASS: explicit arm, successful-version/online-CPU prerequisites, exact AUX/service sequence, cached and partial-failure behavior");
}
