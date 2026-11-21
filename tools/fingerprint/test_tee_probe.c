#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
static char out_buf[8192];
static unsigned calls;
static uint64_t fp_tee_smc(uint64_t f,uint64_t a,uint64_t b,uint64_t *out) {
    assert(f==0xb200000d && a==0x41357 && b==0x30000);
    calls++;*out=0;return 0xffffffffULL;
}
#include "../../xnu/files/bsd/sys/redwood_fp_probe.h"
int main(void) {
    fp_tee_probe(0);assert(!calls && strstr(out_buf,"NOT RUN"));
    fp_tee_probe(2);assert(!calls);
    fp_tee_probe(1);assert(calls==1 && strstr(out_buf,"called once"));
    fp_tee_probe(1);assert(calls==1 && strstr(out_buf,"cached"));
    fp_tee_probe(0);assert(calls==1 && strstr(out_buf,"cached"));
    puts("PASS: no implicit SMC, invalid arm rejected, exact ABI arguments, one call per boot and cached errors");
}
