#include <assert.h>
#include <stdio.h>
#include "../../userland/src/sensor_power_policy.h"
int main(void)
{
    /* Lock-screen pocket/passive requests cannot activate the IR emitter. */
    assert(!redwood_proximity_required(0));
    assert(!redwood_proximity_required(1));
    assert(!redwood_proximity_required(3));
    assert(!redwood_proximity_required(5));
    assert(!redwood_proximity_required(255));
    assert(redwood_proximity_required(2));
    assert(redwood_proximity_required(4));
    puts("sensor power policy: native active/monitoring only passed");
    return 0;
}
