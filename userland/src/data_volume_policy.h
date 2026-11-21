/* SPDX-License-Identifier: MIT
 * Boot may mount existing persistent data. It has no authority to initialize it.
 * The callback interface deliberately provides no format/write operation.
 */
#ifndef REDWOOD_DATA_VOLUME_POLICY_H
#define REDWOOD_DATA_VOLUME_POLICY_H

enum redwood_data_status {
    REDWOOD_DATA_VOLATILE = 0,
    REDWOOD_DATA_MOUNTED = 1,
    REDWOOD_DATA_UNREADABLE = -1,
    REDWOOD_DATA_UNINITIALIZED = -2,
    REDWOOD_DATA_MOUNT_FAILED = -3
};

/* probe: 1 = valid HFSX header, 0 = absent/damaged, negative = I/O failure.
 * mount_existing follows mount(2): zero means success.
 */
static inline int
redwood_mount_persistent_data(int requested, int (*probe)(void), int (*mount_existing)(void))
{
    int state;
    if (!requested) {
        return REDWOOD_DATA_VOLATILE;
    }
    state = probe();
    if (state < 0) {
        return REDWOOD_DATA_UNREADABLE;
    }
    if (state != 1) {
        return REDWOOD_DATA_UNINITIALIZED;
    }
    return mount_existing() == 0 ? REDWOOD_DATA_MOUNTED : REDWOOD_DATA_MOUNT_FAILED;
}

#endif
