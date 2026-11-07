#include "kdrv_revocation.h"
#include "pk.h"

static int rv_eq(const uint8_t *a, const uint8_t *b, uint32_t n) {
    uint8_t d = 0;
    for (uint32_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}
static int rv_zero(const uint8_t *p, uint32_t n) {
    uint8_t v = 0;
    for (uint32_t i = 0; i < n; i++) v |= p[i];
    return v == 0;
}

int kdrv_revocation_apply(const kdrv_revocation_t *s,
                          const kdrv_sig_key_t *trusted, uint32_t count,
                          int64_t now, uint64_t highest,
                          kdrv_sig_key_t *out, uint32_t capacity,
                          uint32_t *out_count, uint64_t *out_generation) {
    if (!s || !trusted || !count || count > KDRV_SIG_MAX_KEYS ||
        !out || !out_count || !out_generation || capacity < count)
        return KDRV_SIG_E_ARGUMENT;
    if (s->magic != KDRV_REVOCATION_MAGIC ||
        s->format != KDRV_REVOCATION_FORMAT || s->size != sizeof(*s) ||
        !s->generation || s->generation <= highest ||
        !s->not_before || s->not_after <= s->not_before ||
        s->not_after > 0x7fffffffffffffffULL ||
        s->revoked_count == 0 || s->revoked_count > KDRV_REVOCATION_MAX_IDS ||
        s->reserved != 0 || rv_zero(s->issuer_key_id, KDRV_SIG_KEY_ID_SIZE))
        return KDRV_SIG_E_FORMAT;
    if (now <= 0) return KDRV_SIG_E_TIME;
    if ((uint64_t)now < s->not_before) return KDRV_SIG_E_NOT_YET;
    if ((uint64_t)now >= s->not_after) return KDRV_SIG_E_EXPIRED;

    const kdrv_sig_key_t *issuer = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (trusted[i].flags & ~KDRV_SIG_KEY_REVOKED) return KDRV_SIG_E_TRUST_STORE;
        for (uint32_t j = 0; j < i; j++)
            if (rv_eq(trusted[i].id, trusted[j].id, KDRV_SIG_KEY_ID_SIZE))
                return KDRV_SIG_E_TRUST_STORE;
        if (rv_eq(trusted[i].id, s->issuer_key_id, KDRV_SIG_KEY_ID_SIZE))
            issuer = &trusted[i];
    }
    if (!issuer) return KDRV_SIG_E_UNKNOWN_KEY;
    if (issuer->flags & KDRV_SIG_KEY_REVOKED) return KDRV_SIG_E_REVOKED;

    for (uint32_t i = 0; i < s->revoked_count; i++) {
        if (rv_zero(s->revoked_ids[i], KDRV_SIG_KEY_ID_SIZE) ||
            rv_eq(s->revoked_ids[i], s->issuer_key_id, KDRV_SIG_KEY_ID_SIZE))
            return KDRV_SIG_E_FORMAT;
        for (uint32_t j = 0; j < i; j++)
            if (rv_eq(s->revoked_ids[i], s->revoked_ids[j], KDRV_SIG_KEY_ID_SIZE))
                return KDRV_SIG_E_FORMAT;
    }
    if (ed25519_verify(issuer->public_key, (const uint8_t *)s,
                       offsetof(kdrv_revocation_t, signature), s->signature) != 0)
        return KDRV_SIG_E_SIGNATURE;

    kdrv_sig_key_t candidate[KDRV_SIG_MAX_KEYS];
    for (uint32_t i = 0; i < count; i++) candidate[i] = trusted[i];
    for (uint32_t i = 0; i < s->revoked_count; i++) {
        uint32_t found = count;
        for (uint32_t j = 0; j < count; j++)
            if (rv_eq(candidate[j].id, s->revoked_ids[i], KDRV_SIG_KEY_ID_SIZE)) {
                found = j;
                break;
            }
        if (found == count) return KDRV_SIG_E_UNKNOWN_KEY;
        candidate[found].flags |= KDRV_SIG_KEY_REVOKED;
    }
    for (uint32_t i = 0; i < count; i++) out[i] = candidate[i];
    *out_count = count;
    *out_generation = s->generation;
    return KDRV_SIG_OK;
}
