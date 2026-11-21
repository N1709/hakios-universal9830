#include "../../xnu/files/iokit/Kernel/SoftwareKeybag.h"
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include <assert.h>
#include <stdio.h>
static unsigned hmac_calls;
static void hmac(const void *key, size_t kn, const void *data, size_t dn, uint8_t out[32]) {
    hmac_calls++;
    CCHmac(kCCHmacAlgSHA256, key, kn, data, dn, out);
}
static void random_bytes(void *out, size_t n) {
    static uint8_t seed = 1;
    uint8_t *b = out;
    for (size_t i = 0; i < n; i++) b[i] = seed++;
}
int main(void) {
    struct swkb_state s, loaded;
    swkb_init(&s, random_bytes);
    assert(swkb_flags(&s) == 6);
    assert(swkb_change(&s, hmac, random_bytes, NULL, 0, "test-only-A", 11, 0) == 0);
    assert(swkb_flags(&s) == 4 && s.record.has_secret == 1);
    uint8_t reference[32];
    assert(CCKeyDerivationPBKDF(kCCPBKDF2, "test-only-A", 11, s.record.salt, 16,
        kCCPRFHmacAlgSHA256, SWKB_ROUNDS, reference, 32) == 0);
    assert(!memcmp(reference, s.record.verifier, 32)); /* independent library oracle */
    swkb_lock(&s);
    assert(swkb_flags(&s) == 5);
    assert(swkb_unlock(&s, hmac, "wrong", 5, 1) == SWKB_BAD_SECRET && s.locked);
    assert(swkb_unlock(&s, hmac, "test-only-A", 11, 2) == 0 && !s.locked);
    struct swkb_record saved = s.record;
    assert(swkb_load(&loaded, &saved, sizeof(saved)) == 0);
    assert(swkb_flags(&loaded) == 1); /* restart requires first authentication */
    unsigned before_empty = hmac_calls;
    assert(swkb_unlock(&loaded, hmac, "", 0, 3) == SWKB_BAD_SECRET && loaded.locked);
    assert(hmac_calls == before_empty && loaded.failures == 1);
    for (unsigned n = 0; n < 4; n++) assert(swkb_unlock(&loaded, hmac, "wrong", 5, 3) == SWKB_BAD_SECRET);
    assert(swkb_unlock(&loaded, hmac, "test-only-A", 11, 3) == SWKB_BACKOFF);
    assert(swkb_unlock(&loaded, hmac, "test-only-A", 11, 4) == 0);
    assert(swkb_change(&loaded, hmac, random_bytes, "wrong", 5, "other", 5, 5) == SWKB_BAD_SECRET);
    assert(!memcmp(&loaded.record, &saved, sizeof(saved)));
    assert(swkb_change(&loaded, hmac, random_bytes, "test-only-A", 11, "test-only-B", 11, 6) == 0);
    assert(swkb_unlock(&loaded, hmac, "test-only-A", 11, 7) == SWKB_BAD_SECRET);
    assert(swkb_unlock(&loaded, hmac, "test-only-B", 11, 8) == 0);
    assert(swkb_change(&loaded, hmac, random_bytes, "test-only-B", 11, NULL, 0, 9) == 0);
    assert(swkb_flags(&loaded) == 6);
    saved.version++;
    assert(swkb_load(&loaded, &saved, sizeof(saved)) == SWKB_BAD_RECORD);
    assert(swkb_load(&loaded, &saved, sizeof(saved)-1) == SWKB_BAD_RECORD);
    assert(swkb_change(&loaded, hmac, random_bytes, NULL, 0, "x", SWKB_MAX_SECRET+1, 10) == SWKB_BAD_SECRET);
    uint8_t binary[] = {0, 1, 0, 2};
    assert(swkb_change(&loaded, hmac, random_bytes, NULL, 0, binary, sizeof(binary), 11) == 0);
    swkb_lock(&loaded);
    assert(swkb_unlock(&loaded, hmac, binary, sizeof(binary)-1, 12) == SWKB_BAD_SECRET);
    assert(swkb_unlock(&loaded, hmac, binary, sizeof(binary), 13) == 0);
    puts("PASS: PBKDF2 oracle, wrong-code rejection, change/remove, lock/reload and rate limiting");
}
