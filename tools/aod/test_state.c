#include "state.h"
#include <assert.h>
#include <stdio.h>
static uint64_t status(unsigned state){return RW_AOD_STATUS_VERSION|RW_AOD_READY|RW_AOD_ENABLED|RW_AOD_NATIVE_ENABLED|RW_AOD_REQUEST_VALID|((uint64_t)state<<8);}
int main(void){
    assert(!rw_aod_idle_suppressed(0));
    assert(!rw_aod_idle_suppressed(0x117)); /* old protocol's target-only status */
    for(unsigned state=0;state<16;state++)assert(rw_aod_idle_suppressed(status(state))==(state<2));
    const uint64_t bits[]={RW_AOD_READY,RW_AOD_ENABLED,RW_AOD_NATIVE_ENABLED,RW_AOD_REQUEST_VALID};
    for(unsigned i=0;i<4;i++)assert(!rw_aod_idle_suppressed(status(1)&~bits[i]));
    /* Awake -> AOD -> repeated timeout -> wake: no lock-button injection in AOD. */
    assert(!rw_aod_idle_suppressed(status(2)));
    assert(rw_aod_idle_suppressed(status(1)));
    assert(rw_aod_idle_suppressed(status(1)|RW_AOD_COMPLETED_VALID|(UINT64_C(1)<<12)));
    assert(!rw_aod_idle_suppressed(status(2)|RW_AOD_COMPLETED_VALID|(UINT64_C(1)<<12)));
    assert(rw_aod_revoke_current(true,5,5,1));
    assert(!rw_aod_revoke_current(true,5,6,1)); /* superseded by another transition */
    assert(!rw_aod_revoke_current(true,5,5,2)); /* wake won */
    assert(!rw_aod_revoke_current(false,5,5,1)); /* disabled before queued callback */
    puts("PASS: native AOD state/idle contract and stale revocation cancellation");
}
