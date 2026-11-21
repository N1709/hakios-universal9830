#include <assert.h>
#include <stdio.h>
#include "redwood_gpu_policy.h"
int main(void) {
    assert(redwood_gpu_rate_verified(0, 572000, 572)); /* unchanged boot PLL */
    assert(redwood_gpu_rate_verified(0, 572000, 571)); /* truncated MHz */
    assert(!redwood_gpu_rate_verified(0, 572000, 570));
    assert(!redwood_gpu_rate_verified(0, 572000, 800)); /* stale clock */
    assert(!redwood_gpu_rate_verified(-1, 572000, 572)); /* failed ACK */
    assert(!redwood_gpu_rate_verified(0, 0, 0));
    assert(!redwood_gpu_rate_verified(0, 572000, 0));
    struct redwood_gpu_policy s = {0};
    assert(!redwood_gpu_policy_update(&s, 87, 0, 1, 1, 0));
    const int t[] = {88,91,94,97,100};
    const unsigned c[] = {702000,572000,455000,377000,260000};
    for (unsigned i=0;i<5;i++) assert(redwood_gpu_policy_update(&s,t[i],90,10+i,1,0)==c[i]);
    assert(redwood_gpu_policy_update(&s,-1,-1,20,1,0)==260000);
    assert(redwood_gpu_policy_update(&s,95,0,30,1,0)==260000);
    assert(redwood_gpu_policy_update(&s,94,0,40,1,0)==377000);
    assert(!redwood_gpu_policy_update(&s,60,0,50,1,0));
    assert(!redwood_gpu_policy_update(&s,80,85,100,0,1));
    assert(!redwood_gpu_policy_update(&s,80,85,10099,0,1));
    assert(redwood_gpu_policy_update(&s,80,85,10100,0,1)==455000);
    assert(redwood_gpu_policy_update(&s,100,90,10200,1,1)==260000);
    assert(redwood_gpu_policy_update(&s,60,0,10300,1,1)==455000);
    assert(redwood_gpu_policy_update(&s,60,0,15299,1,1)==455000);
    assert(!redwood_gpu_policy_update(&s,60,0,15300,1,1));
    assert(!redwood_gpu_policy_update(&s,80,90,16000,0,1));
    assert(!redwood_gpu_policy_update(&s,-1,-1,26000,0,1));
    assert(!redwood_gpu_policy_update(&s,80,90,27000,0,1));
    assert(!redwood_gpu_policy_update(&s,80,90,36999,0,1));
    assert(redwood_gpu_policy_update(&s,80,90,37000,0,1)==455000);
    assert(!redwood_gpu_policy_update(&s,100,90,38000,0,0));
    puts("GPU policy boundaries, hysteresis, dwell, missing readings and disable: passed");
}
