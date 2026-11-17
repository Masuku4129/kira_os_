#include "cbor.h"

/* --- запись --- */
void cbor_w_init(cbor_w* w, uint8_t* buf, size_t cap) { w->p = buf; w->cap = cap; w->n = 0; w->err = 0; }

static void put(cbor_w* w, const void* d, size_t n) {
    if (w->err) return;
    if (n > w->cap - w->n) { w->err = CBOR_ERR_SPACE; return; }
    const uint8_t* s = d;
    for (size_t i = 0; i < n; i++) w->p[w->n + i] = s[i];
    w->n += n;
}
static void head(cbor_w* w, unsigned major, uint64_t v) {      /* наименьшая длина числа: детерминированное кодирование */
    uint8_t b[9]; size_t n;
    if (v < 24) { b[0] = (uint8_t)(major << 5 | v); n = 1; }
    else if (v <= 0xFF) { b[0] = (uint8_t)(major << 5 | 24); b[1] = (uint8_t)v; n = 2; }
    else if (v <= 0xFFFF) { b[0] = (uint8_t)(major << 5 | 25); b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)v; n = 3; }
    else if (v <= 0xFFFFFFFFu) { b[0] = (uint8_t)(major << 5 | 26); for (int i = 0; i < 4; i++) b[1 + i] = (uint8_t)(v >> (24 - 8 * i)); n = 5; }
    else { b[0] = (uint8_t)(major << 5 | 27); for (int i = 0; i < 8; i++) b[1 + i] = (uint8_t)(v >> (56 - 8 * i)); n = 9; }
    put(w, b, n);
}
void cbor_put_uint(cbor_w* w, uint64_t v) { head(w, 0, v); }
void cbor_put_int(cbor_w* w, int64_t v) { if (v >= 0) head(w, 0, (uint64_t)v); else head(w, 1, ~(uint64_t)v); }
void cbor_put_bytes(cbor_w* w, const void* d, size_t n) { head(w, 2, n); put(w, d, n); }

/* строгая проверка UTF-8 (RFC 3629): без «длинных» форм, суррогатов и значений выше U+10FFFF */
static int utf8_ok(const uint8_t* s, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c < 0x80) { i++; continue; }
        size_t len; uint32_t cp;
        if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
        else return 0;
        if (n - i < len) return 0;
        for (size_t k = 1; k < len; k++) { if ((s[i + k] & 0xC0) != 0x80) return 0; cp = cp << 6 | (s[i + k] & 0x3F); }
        if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += len;
    }
    return 1;
}
void cbor_put_text(cbor_w* w, const char* s, size_t n) {
    if (!utf8_ok((const uint8_t*)s, n)) { if (!w->err) w->err = CBOR_ERR_FORMAT; return; }
    head(w, 3, n); put(w, s, n);
}
void cbor_put_cstr(cbor_w* w, const char* s) { size_t n = 0; while (s[n]) n++; cbor_put_text(w, s, n); }
void cbor_put_array(cbor_w* w, uint64_t count) { head(w, 4, count); }
void cbor_put_map(cbor_w* w, uint64_t pairs) { head(w, 5, pairs); }
void cbor_put_tag(cbor_w* w, uint64_t tag) { head(w, 6, tag); }
void cbor_put_bool(cbor_w* w, int v) { uint8_t b = v ? 0xF5 : 0xF4; put(w, &b, 1); }
void cbor_put_null(cbor_w* w) { uint8_t b = 0xF6; put(w, &b, 1); }

/* --- чтение --- */
void cbor_r_init(cbor_r* r, const void* buf, size_t n, int strict) { r->p = buf; r->n = n; r->i = 0; r->strict = strict; }

int cbor_next(cbor_r* r, cbor_item* it) {
    if (r->i >= r->n) return CBOR_ERR_TRUNC;
    uint8_t ib = r->p[r->i];
    unsigned major = ib >> 5, ai = ib & 31;
    size_t pos = r->i + 1;
    uint64_t v = 0;
    if (ai < 24) v = ai;
    else if (ai <= 27) {
        size_t len = (size_t)1 << (ai - 24);
        if (r->n - pos < len) return CBOR_ERR_TRUNC;
        for (size_t k = 0; k < len; k++) v = v << 8 | r->p[pos + k];
        pos += len;
        if (r->strict && major != 7) {                        /* наименьшая длина числа */
            if ((ai == 24 && v < 24) || (ai == 25 && v <= 0xFF) || (ai == 26 && v <= 0xFFFF) || (ai == 27 && v <= 0xFFFFFFFFu)) return CBOR_ERR_STRICT;
        }
    } else if (ai == 31) return r->strict ? CBOR_ERR_STRICT : CBOR_ERR_FORMAT;   /* неопределённая длина не поддерживается */
    else return CBOR_ERR_FORMAT;                               /* 28..30 зарезервированы */
    it->data = 0; it->val = v;
    switch (major) {
    case 0: it->type = CBOR_UINT; break;
    case 1: it->type = CBOR_NINT; break;
    case 2: case 3:
        if (r->n - pos < v) return CBOR_ERR_TRUNC;
        it->type = major == 2 ? CBOR_BYTES : CBOR_TEXT; it->data = r->p + pos;
        if (major == 3 && !utf8_ok(it->data, (size_t)v)) return CBOR_ERR_FORMAT;
        pos += (size_t)v; break;
    case 4: it->type = CBOR_ARRAY; if (v > r->n) return CBOR_ERR_TRUNC; break;     /* элемент занимает >= 1 байта */
    case 5: it->type = CBOR_MAP; if (v > r->n) return CBOR_ERR_TRUNC; break;
    case 6: it->type = CBOR_TAG; break;
    default:                                                    /* 7: простые значения и числа с плавающей точкой */
        if (ai == 20) it->type = CBOR_FALSE; else if (ai == 21) it->type = CBOR_TRUE;
        else if (ai == 22) it->type = CBOR_NULL; else if (ai == 23) it->type = CBOR_UNDEF;
        else if (ai >= 25 && ai <= 27) it->type = CBOR_FLOAT;
        else if (ai == 24) { if (r->strict && v < 32) return CBOR_ERR_STRICT; it->type = CBOR_SIMPLE; }
        else it->type = CBOR_SIMPLE;
        break;
    }
    r->i = pos;
    return CBOR_OK;
}

static int skip_depth(cbor_r* r, int depth) {
    if (depth > CBOR_MAX_DEPTH) return CBOR_ERR_DEPTH;
    cbor_item it; int rc = cbor_next(r, &it);
    if (rc) return rc;
    if (it.type == CBOR_ARRAY || it.type == CBOR_MAP) {
        uint64_t n = it.val * (it.type == CBOR_MAP ? 2 : 1);
        if (n > r->n) return CBOR_ERR_TRUNC;
        for (uint64_t k = 0; k < n; k++) { rc = skip_depth(r, depth + 1); if (rc) return rc; }
    } else if (it.type == CBOR_TAG) return skip_depth(r, depth + 1);
    return CBOR_OK;
}
int cbor_skip(cbor_r* r) { return skip_depth(r, 0); }

int cbor_check(const void* buf, size_t n, int strict) {
    cbor_r r; cbor_r_init(&r, buf, n, strict);
    int rc = cbor_skip(&r);
    if (rc) return rc;
    return r.i == n ? CBOR_OK : CBOR_ERR_FORMAT;                 /* после элемента не должно быть мусора */
}

int cbor_map_find(const void* buf, size_t n, const char* key, cbor_item* out, cbor_r* at) {
    cbor_r r; cbor_r_init(&r, buf, n, 0);
    cbor_item m; int rc = cbor_next(&r, &m);
    if (rc) return rc;
    if (m.type != CBOR_MAP) return CBOR_ERR_TYPE;
    size_t kl = 0; while (key[kl]) kl++;
    for (uint64_t k = 0; k < m.val; k++) {
        cbor_item ki; rc = cbor_next(&r, &ki);
        if (rc) return rc;
        int match = 0;
        if (ki.type == CBOR_TEXT && ki.val == kl) {
            match = 1;
            for (size_t j = 0; j < kl; j++) if (ki.data[j] != (uint8_t)key[j]) { match = 0; break; }
        } else if (ki.type == CBOR_ARRAY || ki.type == CBOR_MAP || ki.type == CBOR_TAG) {   /* составной ключ: пропустить содержимое */
            uint64_t cnt = ki.type == CBOR_TAG ? 1 : ki.val * (ki.type == CBOR_MAP ? 2 : 1);
            if (cnt > r.n) return CBOR_ERR_TRUNC;
            for (uint64_t q = 0; q < cnt; q++) { rc = cbor_skip(&r); if (rc) return rc; }
        }
        if (match) { if (at) *at = r; return cbor_next(&r, out); }
        rc = cbor_skip(&r);
        if (rc) return rc;
    }
    return CBOR_NOT_FOUND;
}
