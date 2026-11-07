#include "kdrv_transition.h"
#include "pk.h"

static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}
static int all_zero(const uint8_t *p, size_t n) {
    uint8_t v = 0;
    for (size_t i = 0; i < n; i++) v |= p[i];
    return v == 0;
}
static void le64(uint8_t *p, uint64_t v) {
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}

int kdrv_transition_verify(const kdrv_transition_t *t,
                           const kdrv_sig_key_t *trusted, uint32_t count,
                           int64_t now, uint64_t highest,
                           kdrv_sig_key_t *out) {
    if (!t || !trusted || !out || !count || count > KDRV_SIG_MAX_KEYS)
        return KDRV_SIG_E_ARGUMENT;
    if (t->magic != KDRV_TRANSITION_MAGIC || t->format != KDRV_TRANSITION_FORMAT ||
        t->size != sizeof(*t) || !t->generation ||
        t->generation <= highest || t->not_before == 0 ||
        t->not_after <= t->not_before || t->not_after > 0x7fffffffffffffffULL)
        return KDRV_SIG_E_FORMAT;
    if (now <= 0) return KDRV_SIG_E_TIME;
    if ((uint64_t)now < t->not_before) return KDRV_SIG_E_NOT_YET;
    if ((uint64_t)now >= t->not_after) return KDRV_SIG_E_EXPIRED;
    if (all_zero(t->old_key_id, sizeof t->old_key_id) ||
        all_zero(t->new_key_id, sizeof t->new_key_id) ||
        all_zero(t->new_public_key, sizeof t->new_public_key) ||
        ct_equal(t->old_key_id, t->new_key_id, KDRV_SIG_KEY_ID_SIZE))
        return KDRV_SIG_E_FORMAT;

    const kdrv_sig_key_t *old = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (trusted[i].flags & ~KDRV_SIG_KEY_REVOKED) return KDRV_SIG_E_TRUST_STORE;
        for (uint32_t j = 0; j < i; j++)
            if (ct_equal(trusted[i].id, trusted[j].id, KDRV_SIG_KEY_ID_SIZE))
                return KDRV_SIG_E_TRUST_STORE;
        if (ct_equal(trusted[i].id, t->old_key_id, KDRV_SIG_KEY_ID_SIZE))
            old = &trusted[i];
    }
    if (!old) return KDRV_SIG_E_UNKNOWN_KEY;
    if (old->flags & KDRV_SIG_KEY_REVOKED) return KDRV_SIG_E_REVOKED;
    for (uint32_t i = 0; i < count; i++)
        if (ct_equal(trusted[i].id, t->new_key_id, KDRV_SIG_KEY_ID_SIZE))
            return KDRV_SIG_E_TRUST_STORE;

    static const uint8_t domain[20] = {
        'K','I','R','A','-','K','D','R','V','-','K','E','Y','R','O','T','-','V','1',0
    };
    uint8_t message[108];
    for (unsigned i = 0; i < sizeof domain; i++) message[i] = domain[i];
    for (unsigned i = 0; i < 16; i++) message[20 + i] = t->old_key_id[i];
    for (unsigned i = 0; i < 16; i++) message[36 + i] = t->new_key_id[i];
    for (unsigned i = 0; i < 32; i++) message[52 + i] = t->new_public_key[i];
    le64(message + 84, t->generation);
    le64(message + 92, t->not_before);
    le64(message + 100, t->not_after);
    if (ed25519_verify(old->public_key, message, sizeof message, t->signature) != 0)
        return KDRV_SIG_E_SIGNATURE;

    for (unsigned i = 0; i < KDRV_SIG_KEY_ID_SIZE; i++) out->id[i] = t->new_key_id[i];
    for (unsigned i = 0; i < sizeof out->public_key; i++) out->public_key[i] = t->new_public_key[i];
    out->flags = 0;
    return KDRV_SIG_OK;
}
