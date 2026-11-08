#include "kdrv_state.h"
#include "immune.h"

static int bytes_eq(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a; const uint8_t* y = (const uint8_t*)b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d == 0;
}
static int all_zero(const uint8_t* p, size_t n) {
    uint8_t v = 0;
    for (size_t i = 0; i < n; i++) v |= p[i];
    return v == 0;
}
static void put32(uint8_t* p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t* p, uint64_t v) { for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t* p) { uint32_t v = 0; for (unsigned i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i); return v; }
static uint64_t get64(const uint8_t* p) { uint64_t v = 0; for (unsigned i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v; }

static int cname_ok(const char* n, unsigned cap) {      /* строка из 1..KDRV_NAME_MAX знаков [a-z0-9_-] с нулём в пределах cap; возвращает длину или 0 */
    unsigned i = 0;
    while (i < cap && n[i]) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
        i++;
    }
    return (i >= 1 && i <= KDRV_NAME_MAX && i < cap) ? i : 0;
}
static int name_ok(const char* n) {                      /* поле записи в файле: после имени только нули */
    unsigned len = cname_ok(n, KDRV_STATE_NAME_SIZE);
    return len && all_zero((const uint8_t*)n + len, KDRV_STATE_NAME_SIZE - len);
}
static int name_eq(const char* a, const char* b) {
    unsigned i = 0;
    while (i < KDRV_STATE_NAME_SIZE && a[i] && a[i] == b[i]) i++;
    return i < KDRV_STATE_NAME_SIZE ? a[i] == b[i] : 1;
}

void kdrv_state_init(kdrv_state_t* st) {
    uint8_t* p = (uint8_t*)st;
    for (size_t i = 0; i < sizeof *st; i++) p[i] = 0;
    st->status = KDRV_STATE_OK;
}

void kdrv_state_mark_io_error(kdrv_state_t* st) { kdrv_state_init(st); st->status = KDRV_STATE_IO; }

void kdrv_state_encode(const kdrv_state_t* st, uint8_t out[KDRV_STATE_FILE_SIZE]) {
    for (unsigned i = 0; i < KDRV_STATE_FILE_SIZE; i++) out[i] = 0;
    put32(out, KDRV_STATE_MAGIC);
    out[4] = (uint8_t)KDRV_STATE_FORMAT; out[5] = 0;
    out[6] = (uint8_t)KDRV_STATE_FILE_SIZE; out[7] = (uint8_t)(KDRV_STATE_FILE_SIZE >> 8);
    put64(out + 8, st->max_time);
    put32(out + 16, st->nfloor);
    put32(out + 20, st->nrevoked);
    for (uint32_t i = 0; i < st->nfloor; i++) {
        uint8_t* e = out + 24 + i * 20u;
        for (unsigned k = 0; k < KDRV_STATE_NAME_SIZE; k++) e[k] = (uint8_t)st->floor[i].name[k];
        put32(e + 16, st->floor[i].floor);
    }
    for (uint32_t i = 0; i < st->nrevoked; i++)
        for (unsigned k = 0; k < KDRV_SIG_KEY_ID_SIZE; k++) out[664 + i * 16u + k] = st->revoked[i][k];
    sha256(out, 920, out + 920);
}

int kdrv_state_decode(kdrv_state_t* st, const void* buf, size_t len) {
    const uint8_t* b = (const uint8_t*)buf;
    kdrv_state_init(st);
    st->status = KDRV_STATE_CORRUPT;
    if (!b || len != KDRV_STATE_FILE_SIZE) return KDRV_STATE_E_CORRUPT;
    uint8_t digest[32];
    sha256(b, 920, digest);
    if (!bytes_eq(digest, b + 920, 32)) return KDRV_STATE_E_CORRUPT;
    if (get32(b) != KDRV_STATE_MAGIC || b[4] != KDRV_STATE_FORMAT || b[5] != 0 ||
        (uint16_t)(b[6] | (b[7] << 8)) != KDRV_STATE_FILE_SIZE) return KDRV_STATE_E_CORRUPT;
    uint64_t mt = get64(b + 8);
    uint32_t nf = get32(b + 16), nr = get32(b + 20);
    if (mt > 0x7FFFFFFFFFFFFFFFULL || nf > KDRV_STATE_MAX_FLOORS || nr > KDRV_STATE_MAX_REVOKED) return KDRV_STATE_E_CORRUPT;
    kdrv_state_t t;
    kdrv_state_init(&t);
    t.max_time = mt; t.nfloor = nf; t.nrevoked = nr;
    for (uint32_t i = 0; i < KDRV_STATE_MAX_FLOORS; i++) {
        const uint8_t* e = b + 24 + i * 20u;
        if (i >= nf) { if (!all_zero(e, 20)) return KDRV_STATE_E_CORRUPT; continue; }
        for (unsigned k = 0; k < KDRV_STATE_NAME_SIZE; k++) t.floor[i].name[k] = (char)e[k];
        if (!name_ok(t.floor[i].name)) return KDRV_STATE_E_CORRUPT;
        for (uint32_t j = 0; j < i; j++) if (name_eq(t.floor[j].name, t.floor[i].name)) return KDRV_STATE_E_CORRUPT;
        t.floor[i].floor = get32(e + 16);
    }
    for (uint32_t i = 0; i < KDRV_STATE_MAX_REVOKED; i++) {
        const uint8_t* e = b + 664 + i * 16u;
        if (i >= nr) { if (!all_zero(e, 16)) return KDRV_STATE_E_CORRUPT; continue; }
        if (all_zero(e, 16)) return KDRV_STATE_E_CORRUPT;
        for (unsigned k = 0; k < KDRV_SIG_KEY_ID_SIZE; k++) t.revoked[i][k] = e[k];
        for (uint32_t j = 0; j < i; j++) if (bytes_eq(t.revoked[j], t.revoked[i], 16)) return KDRV_STATE_E_CORRUPT;
    }
    t.persisted_time = mt;
    *st = t;
    return 0;
}

uint32_t kdrv_state_floor_of(const kdrv_state_t* st, const char* name) {
    for (uint32_t i = 0; i < st->nfloor; i++) if (name_eq(st->floor[i].name, name)) return st->floor[i].floor;
    return 0;
}

int kdrv_state_raise_floor(kdrv_state_t* st, const char* name, uint32_t version) {
    if (!name) return KDRV_STATE_E_ARG;
    char nm[KDRV_STATE_NAME_SIZE] = {0};                 /* безопасная копия: читаем не дальше нуля и не дальше 15 знаков */
    unsigned nl = 0;
    while (nl < sizeof nm - 1 && name[nl]) { nm[nl] = name[nl]; nl++; }
    if (name[nl] || !cname_ok(nm, sizeof nm)) return KDRV_STATE_E_ARG;

    for (uint32_t i = 0; i < st->nfloor; i++) {
        if (!name_eq(st->floor[i].name, name)) continue;
        if (version <= st->floor[i].floor) return 0;
        st->floor[i].floor = version; st->dirty = 1;
        return 1;
    }
    if (version == 0) return 0;
    if (st->nfloor == KDRV_STATE_MAX_FLOORS) return KDRV_STATE_E_FULL;
    kdrv_floor_t* f = &st->floor[st->nfloor++];
    for (unsigned k = 0; k < KDRV_STATE_NAME_SIZE; k++) f->name[k] = 0;
    for (unsigned k = 0; name[k]; k++) f->name[k] = name[k];
    f->floor = version; st->dirty = 1;
    return 1;
}

int kdrv_state_is_revoked(const kdrv_state_t* st, const uint8_t key_id[KDRV_SIG_KEY_ID_SIZE]) {
    for (uint32_t i = 0; i < st->nrevoked; i++) if (bytes_eq(st->revoked[i], key_id, KDRV_SIG_KEY_ID_SIZE)) return 1;
    return 0;
}

int kdrv_state_revoke_key(kdrv_state_t* st, const uint8_t key_id[KDRV_SIG_KEY_ID_SIZE]) {
    if (!key_id || all_zero(key_id, KDRV_SIG_KEY_ID_SIZE)) return KDRV_STATE_E_ARG;
    if (kdrv_state_is_revoked(st, key_id)) return 0;
    if (st->nrevoked == KDRV_STATE_MAX_REVOKED) return KDRV_STATE_E_FULL;
    for (unsigned k = 0; k < KDRV_SIG_KEY_ID_SIZE; k++) st->revoked[st->nrevoked][k] = key_id[k];
    st->nrevoked++; st->dirty = 1;
    return 1;
}

void kdrv_state_reset_time(kdrv_state_t* st, uint64_t now) {
    st->max_time = now <= 0x7FFFFFFFFFFFFFFFULL ? now : 0;
    st->dirty = 1;
}

static void note_time(kdrv_state_t* st, uint64_t now) {
    if (now > st->max_time) st->max_time = now;
    if (st->max_time - st->persisted_time >= KDRV_TIME_PERSIST_STEP) st->dirty = 1;
}

int kdrv_admit(kdrv_state_t* st, const void* pkg, size_t pkg_len, const void* sig, size_t sig_len, int sig_required,
               const kdrv_sig_key_t* keys, uint32_t nkeys, int64_t now_raw,
               kdrv_header_t* h_out, int* auth, int* sig_error) {
    int se = KDRV_SIG_OK;
    int au = KDRV_AUTH_LOCAL;
    kdrv_header_t h;
    int parsed = 0;
    int rc = KDRV_OK;
    uint8_t* hz = (uint8_t*)&h;
    for (size_t i = 0; i < sizeof h; i++) hz[i] = 0;
    if (!st || !pkg || pkg_len > 0xFFFFFFFFu) { rc = KDRV_E_SHORT; goto out; }
    if (st->status != KDRV_STATE_OK) { rc = KDRV_E_STATE; goto out; }
    rc = kdrv_header_check(pkg, (uint32_t)pkg_len, &h);
    if (rc != KDRV_OK) goto out;
    parsed = 1;

    if (!sig && sig_required) { se = KDRV_SIG_E_MISSING; rc = KDRV_E_SIGNATURE; goto out; }
    if (sig) {
        int64_t eff = now_raw;
        if (now_raw > 0) {
            /* Часы ушли назад дальше допуска - отказ; внутри допуска берём не меньше максимума. Время запоминается ДО проверки подписи. */
            if (st->max_time > (uint64_t)now_raw && st->max_time - (uint64_t)now_raw > (uint64_t)KDRV_CLOCK_TOLERANCE) {
                se = KDRV_SIG_E_CLOCK; rc = KDRV_E_SIGNATURE; goto out;
            }
            note_time(st, (uint64_t)now_raw);
            eff = (int64_t)st->max_time;
        }
        if (!keys || nkeys == 0 || nkeys > KDRV_SIG_MAX_KEYS) { se = KDRV_SIG_E_ARGUMENT; rc = KDRV_E_SIGNATURE; goto out; }
        kdrv_sig_key_t eff_keys[KDRV_SIG_MAX_KEYS];     /* отзыв из файла состояния только добавляет флаг, снять его нельзя */
        for (uint32_t i = 0; i < nkeys; i++) {
            eff_keys[i] = keys[i];
            if (kdrv_state_is_revoked(st, keys[i].id)) eff_keys[i].flags |= KDRV_SIG_KEY_REVOKED;
        }
        se = kdrv_sig_verify(pkg, pkg_len, sig, sig_len, eff_keys, nkeys, eff);
        if (se != KDRV_SIG_OK) { rc = KDRV_E_SIGNATURE; goto out; }
        au = KDRV_AUTH_PUBLISHER;
    }
    if (h.version < kdrv_state_floor_of(st, h.name)) rc = KDRV_E_ROLLBACK;
out:
    if (auth) *auth = rc == KDRV_OK ? au : KDRV_AUTH_LOCAL;
    if (sig_error) *sig_error = se;
    if (h_out && parsed) *h_out = h;
    return rc;
}
