#ifndef REDWOOD_SENSOR_POWER_POLICY_H
#define REDWOOD_SENSOR_POWER_POLICY_H
#include <stdbool.h>

/* backboardd 20F66, BKHID proximity service DetectionMode:
 * 0 off, 1 passive, 2 active, 3 pocket, 4 monitoring, 5 passivePocket.
 * Passive/pocket requests occur while merely using the lock screen. They must
 * not illuminate the under-panel IR emitter. Calls/explicit monitoring use 2/4. */
static inline bool redwood_proximity_required(unsigned mode)
{
    return mode == 2 || mode == 4;
}
#endif
