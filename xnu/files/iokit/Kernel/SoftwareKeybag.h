/* Software-only authentication for SEP-less development devices.
 * Does not encrypt existing class keys or attest hardware identity. No secrets
 * are logged. The caller supplies HMAC-SHA256 and random bytes, and serializes
 * calls. Stored verifiers permit offline guessing: this is not SEP protection.
 */
#ifndef REDWOOD_SOFTWARE_KEYBAG_H
#define REDWOOD_SOFTWARE_KEYBAG_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define SWKB_ROUNDS 100000u
#define SWKB_MAX_SECRET 256u
#define SWKB_OK 0
#define SWKB_BAD_SECRET -1
#define SWKB_BAD_RECORD -2
#define SWKB_BACKOFF -3

struct swkb_record {
    uint8_t magic[8];
    uint32_t version, rounds, has_secret, reserved;
    uint8_t uuid[16], salt[16], verifier[32];
};
struct swkb_state {
    struct swkb_record record;
    uint32_t failures;
    int locked, first_unlock;
    uint64_t retry_after;
};
typedef void (*swkb_hmac_fn)(const void *, size_t, const void *, size_t, uint8_t[32]);
typedef void (*swkb_random_fn)(void *, size_t);

static void
swkb_clear(void *p, size_t n)
{
    volatile uint8_t *b = (volatile uint8_t *)p;
    while (n--) *b++ = 0;
}

static int
swkb_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    unsigned different = 0;
    for (size_t i = 0; i < n; i++) different |= a[i] ^ b[i];
    return different == 0;
}

/* PBKDF2-HMAC-SHA256, one 32-byte output block. */
static void
swkb_derive(swkb_hmac_fn hmac, const void *secret, size_t len, const uint8_t salt[16], uint8_t out[32])
{
    uint8_t input[20], u[32], next[32];
    memcpy(input, salt, 16);
    input[16] = input[17] = input[18] = 0; input[19] = 1;
    hmac(secret, len, input, sizeof(input), u);
    memcpy(out, u, 32);
    for (unsigned round = 1; round < SWKB_ROUNDS; round++) {
        hmac(secret, len, u, sizeof(u), next);
        memcpy(u, next, sizeof(u));
        for (unsigned i = 0; i < 32; i++) out[i] ^= u[i];
    }
    swkb_clear(input, sizeof(input));
    swkb_clear(u, sizeof(u));
    swkb_clear(next, sizeof(next));
}

static void
swkb_init(struct swkb_state *s, swkb_random_fn random)
{
    memset(s, 0, sizeof(*s));
    memcpy(s->record.magic, "RW-SWKB1", 8);
    s->record.version = 1;
    s->record.rounds = SWKB_ROUNDS;
    random(s->record.uuid, sizeof(s->record.uuid));
    s->first_unlock = 1;
}

static int
swkb_load(struct swkb_state *s, const void *bytes, size_t len)
{
    struct swkb_record r;
    if (!bytes || len != sizeof(r)) return SWKB_BAD_RECORD;
    memcpy(&r, bytes, sizeof(r));
    if (memcmp(r.magic, "RW-SWKB1", 8) || r.version != 1 ||
        r.rounds != SWKB_ROUNDS || r.has_secret > 1 || r.reserved) {
        swkb_clear(&r, sizeof(r));
        return SWKB_BAD_RECORD;
    }
    memset(s, 0, sizeof(*s));
    s->record = r;
    s->locked = r.has_secret != 0;
    s->first_unlock = !s->locked; /* a saved unlocked state must not survive restart */
    swkb_clear(&r, sizeof(r));
    return SWKB_OK;
}

static int
swkb_check(struct swkb_state *s, swkb_hmac_fn hmac, const void *secret, size_t len, uint64_t now)
{
    uint8_t verifier[32];
    if (len > SWKB_MAX_SECRET || (len && !secret)) return SWKB_BAD_SECRET;
    if (now < s->retry_after) return SWKB_BACKOFF;
    int match;
    if (!s->record.has_secret) {
        match = len == 0;
    } else if (!len) {
        /* Protected records are created only from nonempty secrets. Native
         * nil-passcode probes cannot match: avoid 100,000 HMACs, but retain
         * the same failure accounting and backoff as any incorrect code. */
        match = 0;
    } else {
        swkb_derive(hmac, secret, len, s->record.salt, verifier);
        match = swkb_equal(verifier, s->record.verifier, sizeof(verifier));
        swkb_clear(verifier, sizeof(verifier));
    }
    if (!match) {
        if (s->failures < 255) s->failures++;
        if (s->failures >= 5) {
            unsigned shift = s->failures - 5;
            s->retry_after = now + (shift < 6 ? (1u << shift) : 60u);
        }
        return SWKB_BAD_SECRET;
    }
    s->failures = 0;
    s->retry_after = 0;
    return SWKB_OK;
}

static int
swkb_change(struct swkb_state *s, swkb_hmac_fn hmac, swkb_random_fn random,
    const void *old_secret, size_t old_len, const void *new_secret, size_t new_len, uint64_t now)
{
    if (new_len > SWKB_MAX_SECRET || (new_len && !new_secret)) return SWKB_BAD_SECRET;
    int result = swkb_check(s, hmac, old_secret, old_len, now);
    if (result != SWKB_OK) return result;
    swkb_clear(s->record.salt, sizeof(s->record.salt));
    swkb_clear(s->record.verifier, sizeof(s->record.verifier));
    s->record.has_secret = new_len != 0;
    if (new_len) {
        random(s->record.salt, sizeof(s->record.salt));
        swkb_derive(hmac, new_secret, new_len, s->record.salt, s->record.verifier);
    }
    s->locked = 0;
    s->first_unlock = 1;
    return SWKB_OK;
}

static int
swkb_unlock(struct swkb_state *s, swkb_hmac_fn hmac, const void *secret, size_t len, uint64_t now)
{
    int result = swkb_check(s, hmac, secret, len, now);
    if (result == SWKB_OK) { s->locked = 0; s->first_unlock = 1; }
    return result;
}

static void
swkb_lock(struct swkb_state *s)
{
    if (s->record.has_secret) s->locked = 1;
}

static unsigned
swkb_flags(const struct swkb_state *s)
{
    return (s->locked ? 1u : 0u) | (s->record.has_secret ? 0u : 2u) | (s->first_unlock ? 4u : 0u);
}
#endif
