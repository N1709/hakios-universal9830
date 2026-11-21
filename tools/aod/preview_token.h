#ifndef REDWOOD_AOD_PREVIEW_TOKEN_H
#define REDWOOD_AOD_PREVIEW_TOKEN_H
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#define RW_AOD_DIRECTORY "/var/tmp/redwood-aod-preview"
#define RW_AOD_BEGIN "com.redwood.aod-preview.begin"
#define RW_AOD_END "com.redwood.aod-preview.end"
#define RW_AOD_STATUS "com.redwood.aod-preview.status"
#define RW_AOD_MAGIC UINT64_C(0x31444f415752)
struct rw_aod_token { uint64_t magic, nonce; double expires; };
static inline bool rw_aod_token_valid(struct rw_aod_token t,double now,uint64_t used){
    return t.magic==RW_AOD_MAGIC && t.nonce && t.nonce!=used && isfinite(now) &&
        isfinite(t.expires) && t.expires>now && t.expires-now<=31;
}
#endif
