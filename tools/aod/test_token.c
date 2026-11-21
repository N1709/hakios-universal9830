#include "preview_token.h"
#include <assert.h>
#include <stdio.h>
int main(void){
    struct rw_aod_token t={.magic=RW_AOD_MAGIC,.nonce=42,.expires=130};
    assert(rw_aod_token_valid(t,100,0));
    assert(!rw_aod_token_valid(t,100,42));
    assert(!rw_aod_token_valid(t,130,0));
    assert(!rw_aod_token_valid(t,98,0));
    t.expires=INFINITY;assert(!rw_aod_token_valid(t,100,0));
    t.expires=130;t.nonce=0;assert(!rw_aod_token_valid(t,100,0));
    t.nonce=42;t.magic=0;assert(!rw_aod_token_valid(t,100,0));
    puts("PASS: expiry, replay, excessive lifetime, nonfinite expiry, nonce and magic");
}
