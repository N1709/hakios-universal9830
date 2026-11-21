#include <assert.h>
#include "../../../userland/src/data_volume_policy.h"

static int probe_result, mount_result, probes, mounts;
static int probe(void) { ++probes; return probe_result; }
static int mount_existing(void) { ++mounts; return mount_result; }

int main(void)
{
    assert(redwood_mount_persistent_data(0, probe, mount_existing) == REDWOOD_DATA_VOLATILE);
    assert(probes == 0 && mounts == 0);
    /* I/O failures and absent/corrupt headers must never reach mount or initialization. */
    probe_result = -1;
    assert(redwood_mount_persistent_data(1, probe, mount_existing) == REDWOOD_DATA_UNREADABLE);
    assert(mounts == 0);
    probe_result = 0;
    assert(redwood_mount_persistent_data(1, probe, mount_existing) == REDWOOD_DATA_UNINITIALIZED);
    assert(mounts == 0);
    probe_result = 1;
    mount_result = -1;
    assert(redwood_mount_persistent_data(1, probe, mount_existing) == REDWOOD_DATA_MOUNT_FAILED);
    assert(mounts == 1);
    mount_result = 0;
    assert(redwood_mount_persistent_data(1, probe, mount_existing) == REDWOOD_DATA_MOUNTED);
    assert(mounts == 2);
    return 0;
}
