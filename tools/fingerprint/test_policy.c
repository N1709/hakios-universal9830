#include "policy.h"
#include <assert.h>
#include <stdio.h>
static const uint8_t nonce[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
static struct fp_policy ready(void) {
    struct fp_policy s;fp_policy_init(&s);fp_policy_backend(&s,true,true,9);
    assert(fp_policy_passcode_verified(&s,7,100));return s;
}
int main(void) {
    struct fp_policy s;fp_policy_init(&s);
    assert(!fp_policy_begin(&s,7,100,nonce)); /* absent backend / startup */
    fp_policy_backend(&s,true,true,9);
    assert(!fp_policy_begin(&s,7,100,nonce)); /* first unlock needs code */
    assert(fp_policy_passcode_verified(&s,7,100));
    uint8_t zero[16]={0},wrong[16]={2};
    assert(!fp_policy_begin(&s,7,100,zero));
    assert(fp_policy_begin(&s,7,101,nonce));
    assert(fp_policy_result(&s,7,9,102,nonce,FP_PRESENT)==FP_WAIT);
    assert(s.pending && !s.failures);
    assert(fp_policy_result(&s,7,9,103,nonce,FP_VERIFIED_MATCH)==FP_ELIGIBLE_FOR_NATIVE_UNLOCK);
    assert(fp_policy_result(&s,7,9,103,nonce,FP_VERIFIED_MATCH)==FP_DENY); /* replay */
    s=ready();assert(fp_policy_begin(&s,7,101,nonce));
    assert(fp_policy_result(&s,7,9,102,nonce,FP_NO_MATCH)==FP_DENY);
    assert(s.failures==1 && !fp_policy_begin(&s,7,103,nonce));
    assert(fp_policy_passcode_verified(&s,7,104) && !s.failures);
    assert(fp_policy_begin(&s,7,105,nonce));fp_policy_cancel(&s);
    assert(fp_policy_result(&s,7,9,106,nonce,FP_VERIFIED_MATCH)==FP_DENY);
    s=ready();assert(fp_policy_begin(&s,7,101,nonce));
    assert(fp_policy_result(&s,7,9,102,wrong,FP_VERIFIED_MATCH)==FP_DENY);
    assert(!s.passcode_verified);
    s=ready();assert(fp_policy_begin(&s,7,101,nonce));
    assert(fp_policy_result(&s,7,10,102,nonce,FP_VERIFIED_MATCH)==FP_DENY);
    s=ready();assert(!fp_policy_begin(&s,8,101,nonce)); /* bag changed */
    s=ready();assert(!fp_policy_begin(&s,7,99,nonce)); /* clock backwards */
    s=ready();assert(!fp_policy_begin(&s,7,100+FP_PASSCODE_LIFETIME,nonce));
    s=ready();assert(fp_policy_begin(&s,7,101,nonce));
    assert(fp_policy_result(&s,7,9,106,nonce,FP_VERIFIED_MATCH)==FP_DENY);
    s=ready();fp_policy_backend(&s,true,true,10);assert(!s.passcode_verified);
    s=ready();fp_policy_backend(&s,false,true,9);assert(!s.passcode_verified);
    s=ready();fp_policy_backend(&s,true,false,9);assert(!s.passcode_verified);
    s=ready();assert(fp_policy_begin(&s,7,101,nonce));
    assert(fp_policy_result(&s,7,9,102,nonce,FP_BACKEND_ERROR)==FP_DENY);
    assert(!s.passcode_verified);
    puts("PASS: no backend/startup unlock, presence rejection, passcode fallback, one-shot request, stale/bag/enrollment binding, cancel and expiry");
}
