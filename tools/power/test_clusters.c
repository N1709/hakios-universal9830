#include "../../xnu/files/bsd/sys/redwood_power_policy.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    struct redwood_cooling_state s[4] = {0};
    int cool[] = {60, 60, 60, 60}, t[4];
    const int threshold[] = {76, 80, 82, 80};
    const unsigned bit[] = {4, 2, 1, 8};
    for (unsigned z = 0; z < 4; z++) {
        memset(s, 0, sizeof(s));
        memcpy(t, cool, sizeof(t));
        t[z] = threshold[z] - 1;
        assert(redwood_cooling_mask(s, t, 0) == 0);
        t[z]++;
        assert(redwood_cooling_mask(s, t, 1) == bit[z]);
        assert(redwood_cooling_mask(s, cool, 2) == bit[z]);
        assert(redwood_cooling_mask(s, cool, 31) == bit[z]);
        assert(redwood_cooling_mask(s, cool, 32) == 0);
    }
    memset(s, 0, sizeof(s));
    int broad[] = {76, 80, 60, 60};
    assert(redwood_cooling_mask(s, broad, 0) == (4|2));
    int m5_warm[] = {70, 60, 60, 60};
    assert(redwood_cooling_mask(s, m5_warm, 1) == (4|2));
    assert(redwood_cooling_mask(s, m5_warm, 31) == 4); /* A76 recovers independently */
    assert(redwood_cooling_mask(s, cool, 32) == 4);
    assert(redwood_cooling_mask(s, cool, 62) == 0);
    memset(s, 0, sizeof(s));
    int missing[] = {-1, 60, 60, 60};
    assert(redwood_cooling_mask(s, missing, 0) == 4); /* one failed sensor only caps its cluster */
    assert(redwood_cooling_mask(s, cool, 1) == 4);
    assert(redwood_cooling_mask(s, missing, 20) == 4); /* recovery interrupted */
    assert(redwood_cooling_mask(s, cool, 21) == 4);
    assert(redwood_cooling_mask(s, cool, 51) == 0);
    int all_missing[] = {-1,-1,-1,-1};
    assert(redwood_cooling_mask(s, all_missing, 60) == REDWOOD_COOL_ALL);
    puts("PASS: native independent caps, independent recovery, memory isolation and sensor failures");
}
