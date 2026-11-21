/* Samsung-derived GPU policy. Threshold progression and dwell are our policy,
 * not Samsung IPA. No timer or hardware access; caller supplies monotonic ms. */
#ifndef REDWOOD_GPU_POLICY_H
#define REDWOOD_GPU_POLICY_H
#include <stdint.h>
/* PLL readback is integer MHz. A rate already set before the request is valid. */
static inline int
redwood_gpu_rate_verified(int error, uint32_t requested_khz, uint32_t actual_mhz)
{
    uint32_t asked = requested_khz / 1000;
    uint32_t delta = actual_mhz > asked ? actual_mhz - asked : asked - actual_mhz;
    return !error && requested_khz && actual_mhz && delta <= 1;
}
struct redwood_gpu_policy {
    unsigned level;
    int sustained;
    uint64_t busy_since, quiet_since;
};
static inline uint32_t
redwood_gpu_policy_update(struct redwood_gpu_policy *s, int temp, int busy,
    uint64_t now_ms, int ladder, int sustain)
{
    static const uint32_t caps[] = { 800000, 702000, 572000, 455000, 377000, 260000 };
    unsigned want = 0;
    /* Invalid samples retain an active cap; never infer that a hot GPU cooled. */
    if (!ladder) {
        s->level = 0;
    } else if (temp >= 0) {
        if (temp >= 88) {
            want = 1 + (unsigned)(temp - 88) / 3;
            if (want > 5) want = 5;
        }
        if (want > s->level) s->level = want;
        while (s->level && temp < 88 + (int)(s->level - 1) * 3 - 5)
            --s->level;
    }
    if (!sustain) {
        s->sustained = 0;
        s->busy_since = s->quiet_since = 0;
    } else if (temp < 0 || busy < 0) {
        /* Unknown time is not evidence for either dwell interval. */
        s->busy_since = s->quiet_since = 0;
    } else if (temp >= 78 && busy >= 85) {
        s->quiet_since = 0;
        if (!s->busy_since) s->busy_since = now_ms;
        if (now_ms - s->busy_since >= 10000) s->sustained = 1;
    } else {
        s->busy_since = 0;
        if (temp < 73 || busy < 75) {
            if (!s->quiet_since) s->quiet_since = now_ms;
            if (now_ms - s->quiet_since >= 5000) s->sustained = 0;
        } else {
            s->quiet_since = 0;
        }
    }
    uint32_t cap = s->level ? caps[s->level] : 0;
    if (s->sustained && (!cap || cap > 455000)) cap = 455000;
    return cap;
}
#endif
