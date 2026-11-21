/* Pure thermal policy shared with host tests. Times are monotonic seconds. */
#ifndef REDWOOD_CLUSTER_COOLING_H
#define REDWOOD_CLUSTER_COOLING_H
#include <stdint.h>

/* Bits use domain order: A55, A76, M5, GPU; memory is never inferred from their heat. */
#define REDWOOD_COOL_ALL 15u
struct redwood_cooling_state {
    int capped, cooling;
    uint64_t cool_since;
};

static int
redwood_cooling_update(struct redwood_cooling_state *s, int valid, int hot, int cool, uint64_t now)
{
    if (!valid || hot) {
        s->capped = 1;
        s->cooling = 0;
    } else if (cool && s->capped) {
        if (!s->cooling) {
            s->cool_since = now;
            s->cooling = 1;
        } else if (now - s->cool_since >= 30) {
            s->capped = 0;
            s->cooling = 0;
        }
    } else {
        s->cooling = 0;
    }
    return s->capped;
}

static unsigned
redwood_cooling_mask(struct redwood_cooling_state state[4], const int temp[4], uint64_t now)
{
    /* TMU order: M5, A76, A55, GPU. */
    static const int engage[4] = {76, 80, 82, 80};
    static const int recover[4] = {68, 72, 74, 72};
    static const unsigned bit[4] = {2, 1, 0, 3};
    unsigned mask = 0;

    for (unsigned z = 0; z < 4; z++) {
        int valid = temp[z] >= 0 && temp[z] <= 125;
        int hot = valid && temp[z] >= engage[z];
        int cool = valid && temp[z] <= recover[z];
        if (redwood_cooling_update(&state[bit[z]], valid, hot, cool, now)) {
            mask |= 1u << bit[z];
        }
    }
    /* No dedicated memory telemetry: do not infer a memory cap from these zones. */
    return mask;
}
#endif
