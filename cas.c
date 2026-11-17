#include "cas.h"
#include "sha256.h"

static void h2(uint8_t out[CAS_HASH], uint8_t tag, const void* a, size_t an, const void* b, size_t bn) {
    sha256_ctx_t c; sha256_init(&c);
    sha256_update(&c, &tag, 1);
    if (an) sha256_update(&c, a, an);
    if (bn) sha256_update(&c, b, bn);
    sha256_final(&c, out);
}
static int hash_eq(const uint8_t* a, const uint8_t* b) { uint8_t d = 0; for (int i = 0; i < CAS_HASH; i++) d |= a[i] ^ b[i]; return d == 0; }
static void cpy(void* d, const void* s, size_t n) { uint8_t* x = d; const uint8_t* y = s; for (size_t i = 0; i < n; i++) x[i] = y[i]; }
static void mv(uint8_t* d, const uint8_t* s, size_t n) { if (d < s) for (size_t i = 0; i < n; i++) d[i] = s[i]; else for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1]; }

void merkle_leaf(const void* data, size_t len, uint8_t out[CAS_HASH]) { h2(out, 0x00, data, len, 0, 0); }

static size_t split(size_t n) { size_t k = 1; while (k * 2 < n) k *= 2; return k; }        /* наибольшая степень двойки, строго меньшая n */

static void mth(const uint8_t (*l)[CAS_HASH], size_t n, uint8_t out[CAS_HASH]) {
    if (n == 1) { for (int i = 0; i < CAS_HASH; i++) out[i] = l[0][i]; return; }
    size_t k = split(n); uint8_t a[CAS_HASH], b[CAS_HASH];
    mth(l, k, a); mth(l + k, n - k, b);
    h2(out, 0x01, a, CAS_HASH, b, CAS_HASH);
}
void merkle_root(const uint8_t (*leaves)[CAS_HASH], size_t n, uint8_t out[CAS_HASH]) {
    if (n == 0) { sha256_ctx_t c; sha256_init(&c); sha256_final(&c, out); return; }
    mth(leaves, n, out);
}

static int path_rec(const uint8_t (*l)[CAS_HASH], size_t n, size_t m, uint8_t (*path)[CAS_HASH], size_t cap, size_t* cnt) {
    if (n == 1) return 0;
    size_t k = split(n); uint8_t sib[CAS_HASH];
    if (m < k) {
        int rc = path_rec(l, k, m, path, cap, cnt); if (rc) return rc;
        mth(l + k, n - k, sib);
    } else {
        int rc = path_rec(l + k, n - k, m - k, path, cap, cnt); if (rc) return rc;
        mth(l, k, sib);
    }
    if (*cnt >= cap) return CAS_ERR_SPACE;
    cpy(path[(*cnt)++], sib, CAS_HASH);
    return 0;
}
int merkle_path(const uint8_t (*leaves)[CAS_HASH], size_t n, size_t index, uint8_t (*path)[CAS_HASH], size_t path_cap) {
    if (!leaves || n == 0 || index >= n || !path) return CAS_ERR_ARG;
    size_t cnt = 0; int rc = path_rec(leaves, n, index, path, path_cap, &cnt);
    return rc ? rc : (int)cnt;
}
int merkle_verify(const uint8_t leaf[CAS_HASH], size_t index, size_t n, const uint8_t (*path)[CAS_HASH], size_t plen, const uint8_t root[CAS_HASH]) {
    if (n == 0 || index >= n) return CAS_ERR_ARG;
    /* RFC 9162, 2.1.3.2: идём от листа к корню, fn - номер листа, sn - последний номер в поддереве */
    size_t fn = index, sn = n - 1, used = 0; uint8_t r[CAS_HASH];
    cpy(r, leaf, CAS_HASH);
    while (sn > 0) {
        if (used >= plen) return CAS_ERR_CORRUPT;
        if ((fn & 1) || fn == sn) {
            h2(r, 0x01, path[used], CAS_HASH, r, CAS_HASH);
            if (!(fn & 1)) { while (!(fn & 1) && fn != 0) { fn >>= 1; sn >>= 1; } }
        } else h2(r, 0x01, r, CAS_HASH, path[used], CAS_HASH);
        used++; fn >>= 1; sn >>= 1;
    }
    return used == plen && hash_eq(r, root) ? 0 : CAS_ERR_CORRUPT;
}

/* --- хранилище --- */
void cas_init(cas_store* s, cas_entry* entries, size_t max_entries, uint8_t* arena, size_t arena_cap) {
    s->e = entries; s->max = max_entries; s->arena = arena; s->cap = arena_cap; s->top = 0; s->count = 0;
    for (size_t i = 0; i < max_entries; i++) entries[i].used = 0;
}
static void content_hash(const void* d, size_t n, uint8_t out[CAS_HASH]) { sha256_ctx_t c; sha256_init(&c); if (n) sha256_update(&c, d, n); sha256_final(&c, out); }
static cas_entry* find(const cas_store* s, const uint8_t h[CAS_HASH]) {
    for (size_t i = 0; i < s->max; i++) if (s->e[i].used && hash_eq(s->e[i].hash, h)) return &s->e[i];
    return 0;
}
int cas_put(cas_store* s, const void* data, size_t len, uint8_t hash[CAS_HASH]) {
    if (!s || (!data && len) || len > 0xFFFFFFFFu) return CAS_ERR_ARG;
    uint8_t h[CAS_HASH]; content_hash(data, len, h);
    cas_entry* e = find(s, h);
    if (e) { e->refs++; cpy(hash, h, CAS_HASH); return CAS_OK; }
    if (len > s->cap - s->top) return CAS_ERR_FULL;
    for (size_t i = 0; i < s->max; i++) if (!s->e[i].used) {
        cpy(s->arena + s->top, data, len);
        cpy(s->e[i].hash, h, CAS_HASH); s->e[i].off = (uint32_t)s->top; s->e[i].len = (uint32_t)len; s->e[i].refs = 1; s->e[i].used = 1;
        s->top += len; s->count++; cpy(hash, h, CAS_HASH);
        return CAS_OK;
    }
    return CAS_ERR_FULL;
}
int cas_get(const cas_store* s, const uint8_t hash[CAS_HASH], const uint8_t** data, size_t* len) {
    cas_entry* e = find(s, hash);
    if (!e) return CAS_ERR_NOTFOUND;
    uint8_t h[CAS_HASH]; content_hash(s->arena + e->off, e->len, h);
    if (!hash_eq(h, hash)) return CAS_ERR_CORRUPT;                       /* данные в хранилище изменились */
    *data = s->arena + e->off; *len = e->len;
    return CAS_OK;
}
int cas_has(const cas_store* s, const uint8_t hash[CAS_HASH]) { return find(s, hash) != 0; }
int cas_release(cas_store* s, const uint8_t hash[CAS_HASH]) {
    cas_entry* e = find(s, hash);
    if (!e) return CAS_ERR_NOTFOUND;
    if (e->refs == 0) return CAS_ERR_ARG;
    e->refs--;
    return CAS_OK;
}
size_t cas_gc(cas_store* s) {
    size_t freed = 0;
    for (size_t i = 0; i < s->max; i++) if (s->e[i].used && s->e[i].refs == 0) {
        uint32_t off = s->e[i].off, len = s->e[i].len;
        mv(s->arena + off, s->arena + off + len, s->top - off - len);          /* уплотнение арены */
        for (size_t j = 0; j < s->max; j++) if (s->e[j].used && s->e[j].off > off) s->e[j].off -= len;
        s->top -= len; s->e[i].used = 0; s->count--; freed += len;
    }
    return freed;
}

/* манифест: "KMF1", полная длина (u64 LE), размер куска (u32 LE), затем хэши кусков */
#define MF_HEAD 16
size_t cas_file_scratch(size_t len, size_t chunk) {
    size_t nch = (chunk && len) ? (len + chunk - 1) / chunk : 0;
    return MF_HEAD + nch * CAS_HASH * 2;                                  /* манифест + листья дерева */
}
int cas_put_file(cas_store* s, const void* data, size_t len, size_t chunk, uint8_t* scratch, size_t scratch_cap,
                 uint8_t manifest[CAS_HASH], uint8_t root[CAS_HASH]) {
    if (!s || (!data && len) || chunk == 0 || chunk > 0x10000000u || !scratch) return CAS_ERR_ARG;
    size_t nch = len ? (len + chunk - 1) / chunk : 0;
    if (scratch_cap < cas_file_scratch(len, chunk)) return CAS_ERR_SPACE;
    size_t mfn = MF_HEAD + nch * CAS_HASH;
    uint8_t* mf = scratch; uint8_t (*lv)[CAS_HASH] = (uint8_t (*)[CAS_HASH])(scratch + mfn);
    mf[0] = 'K'; mf[1] = 'M'; mf[2] = 'F'; mf[3] = '1';
    for (int i = 0; i < 8; i++) mf[4 + i] = (uint8_t)((uint64_t)len >> (8 * i));
    for (int i = 0; i < 4; i++) mf[12 + i] = (uint8_t)(chunk >> (8 * i));
    const uint8_t* p = data;
    for (size_t i = 0; i < nch; i++) {
        size_t n = len - i * chunk < chunk ? len - i * chunk : chunk;
        int rc = cas_put(s, p + i * chunk, n, mf + MF_HEAD + i * CAS_HASH);
        if (rc) return rc;                                                  /* уже положенные куски остаются с refs: вызывающий их освободит */
        merkle_leaf(mf + MF_HEAD + i * CAS_HASH, CAS_HASH, lv[i]);
    }
    if (root) merkle_root((const uint8_t (*)[CAS_HASH])lv, nch, root);
    return cas_put(s, mf, mfn, manifest);
}
int cas_get_file(const cas_store* s, const uint8_t manifest[CAS_HASH], void* out, size_t cap, size_t* len) {
    const uint8_t* mf; size_t mn;
    int rc = cas_get(s, manifest, &mf, &mn);
    if (rc) return rc;
    if (mn < MF_HEAD || mf[0] != 'K' || mf[1] != 'M' || mf[2] != 'F' || mf[3] != '1') return CAS_ERR_CORRUPT;
    uint64_t total = 0; for (int i = 0; i < 8; i++) total |= (uint64_t)mf[4 + i] << (8 * i);
    uint32_t chunk = 0; for (int i = 0; i < 4; i++) chunk |= (uint32_t)mf[12 + i] << (8 * i);
    size_t nch = (mn - MF_HEAD) / CAS_HASH;
    if (chunk == 0 || (mn - MF_HEAD) % CAS_HASH || nch != (total ? (total + chunk - 1) / chunk : 0)) return CAS_ERR_CORRUPT;
    if (total > cap) return CAS_ERR_SPACE;
    uint8_t* o = out; uint64_t done = 0;
    for (size_t i = 0; i < nch; i++) {
        const uint8_t* d; size_t dn;
        rc = cas_get(s, mf + MF_HEAD + i * CAS_HASH, &d, &dn);
        if (rc) return rc;
        uint64_t want = total - done < chunk ? total - done : chunk;
        if (dn != want) return CAS_ERR_CORRUPT;
        cpy(o + done, d, dn); done += dn;
    }
    *len = (size_t)total;
    return CAS_OK;
}
