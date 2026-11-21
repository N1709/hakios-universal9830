/* Shared native-service diagnostics and idle-timer contract. No hardware writes. */
#ifndef REDWOOD_AOD_STATE_H
#define REDWOOD_AOD_STATE_H
#include <stdbool.h>
#include <stdint.h>
#define RW_AOD_SERVICE_STATUS "com.redwood.alwayson.status"
#define RW_AOD_STATUS_VERSION (UINT64_C(0xa0d1) << 32)
#define RW_AOD_READY UINT64_C(1)
#define RW_AOD_ENABLED UINT64_C(2)
#define RW_AOD_NATIVE_ENABLED UINT64_C(4)
#define RW_AOD_REQUEST_VALID UINT64_C(8)
#define RW_AOD_COMPLETED_VALID UINT64_C(128)
static inline bool rw_aod_inactive(long long state){return state==0||state==1;}
static inline bool rw_aod_idle_suppressed(uint64_t flags){
    const uint64_t required=RW_AOD_READY|RW_AOD_ENABLED|RW_AOD_NATIVE_ENABLED|RW_AOD_REQUEST_VALID;
    return (flags>>32)==(RW_AOD_STATUS_VERSION>>32)&&(flags&required)==required&&
        rw_aod_inactive((long long)((flags>>8)&15));
}
static inline bool rw_aod_revoke_current(bool enabled,uint64_t scheduled,uint64_t current,long long state){
    return enabled&&scheduled==current&&rw_aod_inactive(state);
}
#endif
