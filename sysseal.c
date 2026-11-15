#include "sysseal.h"
#include "cbor.h"
#include "cas.h"

static void put_le64(uint8_t* o, uint64_t v) { for (int i = 0; i < 8; i++) o[i] = (uint8_t)(v >> (8 * i)); }

static size_t mem_len(const char* s) { size_t n = 0; while (s[n]) n++; return n; }

static int leaf_of(const char* path, size_t plen, uint64_t size, const uint8_t hash[32], uint8_t out[32]) {
    uint8_t buf[SEAL_NAME_MAX + 1 + 32 + 8];
    if (plen == 0 || plen >= SEAL_NAME_MAX) return -1;
    size_t o = 0;
    for (size_t i = 0; i < plen; i++) buf[o++] = (uint8_t)path[i];
    buf[o++] = 0;
    for (int i = 0; i < 32; i++) buf[o++] = hash[i];
    put_le64(buf + o, size); o += 8;
    merkle_leaf(buf, o, out);
    return 0;
}

void seal_leaf(const seal_entry* e, uint8_t out[32]) { (void)leaf_of(e->path, mem_len(e->path), e->size, e->hash, out); }

size_t seal_build(const seal_entry* e, size_t n, uint8_t* out, size_t cap) {
    if (n > SEAL_MAX_FILES) return 0;
    static uint8_t leaves[SEAL_MAX_FILES][32];     /* утилита однопоточная; ядро эту функцию не вызывает */
    uint8_t root[32];
    for (size_t i = 0; i < n; i++) {
        if (mem_len(e[i].path) >= SEAL_NAME_MAX || e[i].path[0] != '/') return 0;
        seal_leaf(&e[i], leaves[i]);
    }
    merkle_root((const uint8_t (*)[32])leaves, n, root);
    cbor_w w; cbor_w_init(&w, out, cap);
    cbor_put_map(&w, 4);
    cbor_put_cstr(&w, "n");     cbor_put_uint(&w, n);
    cbor_put_cstr(&w, "v");     cbor_put_uint(&w, 1);
    cbor_put_cstr(&w, "root");  cbor_put_bytes(&w, root, 32);
    cbor_put_cstr(&w, "files"); cbor_put_array(&w, n);
    for (size_t i = 0; i < n; i++) {
        cbor_put_array(&w, 3);
        cbor_put_cstr(&w, e[i].path); cbor_put_uint(&w, e[i].size); cbor_put_bytes(&w, e[i].hash, 32);
    }
    return w.err ? 0 : w.n;
}

int seal_check(const uint8_t* m, size_t n, seal_hash_fn fn, void* ctx, uint8_t (*leaves)[32],
               void (*bad)(void* ctx, const char* path, const char* why), seal_report* rep) {
    *rep = (seal_report){0};
    int r = cbor_check(m, n, 1);
    if (r != CBOR_OK) return rep->format = r;
    cbor_item it; cbor_r at;
    if (cbor_map_find(m, n, "v", &it, &at) != 0 || it.type != CBOR_UINT || it.val != 1) return rep->format = -100;
    if (cbor_map_find(m, n, "root", &it, &at) != 0 || it.type != CBOR_BYTES || it.val != 32) return rep->format = -100;
    for (int i = 0; i < 32; i++) rep->root[i] = it.data[i];
    if (cbor_map_find(m, n, "files", &it, &at) != 0 || it.type != CBOR_ARRAY || it.val > SEAL_MAX_FILES) return rep->format = -100;
    uint64_t count = it.val;
    if (cbor_next(&at, &it) || it.type != CBOR_ARRAY || it.val != count) return rep->format = -100;   /* at стоит на заголовке массива */
    for (uint64_t k = 0; k < count; k++) {
        cbor_item a, p, sz, h;
        if (cbor_next(&at, &a) || a.type != CBOR_ARRAY || a.val != 3) return rep->format = -100;
        if (cbor_next(&at, &p) || p.type != CBOR_TEXT || p.val == 0 || p.val >= SEAL_NAME_MAX) return rep->format = -100;
        if (cbor_next(&at, &sz) || sz.type != CBOR_UINT) return rep->format = -100;
        if (cbor_next(&at, &h) || h.type != CBOR_BYTES || h.val != 32) return rep->format = -100;
        char path[SEAL_NAME_MAX];
        for (uint64_t i = 0; i < p.val; i++) path[i] = (char)p.data[i];
        path[p.val] = 0;
        if (leaf_of(path, (size_t)p.val, sz.val, h.data, leaves[k])) return rep->format = -100;
        rep->files++;
        uint64_t size = 0; uint8_t d[32];
        if (fn(ctx, path, &size, d)) { rep->missing++; if (bad) bad(ctx, path, "нет файла"); continue; }
        int same = size == sz.val;
        for (int i = 0; i < 32; i++) if (d[i] != h.data[i]) same = 0;
        if (same) rep->ok++; else { rep->changed++; if (bad) bad(ctx, path, "изменён"); }
    }
    uint8_t root[32];
    merkle_root((const uint8_t (*)[32])leaves, (size_t)count, root);
    rep->root_ok = 1;
    for (int i = 0; i < 32; i++) if (root[i] != rep->root[i]) rep->root_ok = 0;
    uint64_t declared;
    if (cbor_map_find(m, n, "n", &it, &at) != 0 || it.type != CBOR_UINT) return rep->format = -100;
    declared = it.val;
    if (declared != count) rep->root_ok = 0;
    return 0;
}
