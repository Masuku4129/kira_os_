#include "zstd.h"

/* ---- XXH64: контрольная сумма содержимого кадра ---- */
#define P1 0x9E3779B185EBCA87ULL
#define P2 0xC2B2AE3D27D4EB4FULL
#define P3 0x165667B19E3779F9ULL
#define P4 0x85EBCA77C2B2AE63ULL
#define P5 0x27D4EB2F165667C5ULL
static uint64_t rol64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static uint64_t le64(const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }
static uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t xround(uint64_t acc, uint64_t in) { return rol64(acc + in * P2, 31) * P1; }
static uint64_t xmerge(uint64_t h, uint64_t v) { return (h ^ xround(0, v)) * P1 + P4; }
static uint64_t xxh64(const uint8_t* p, size_t n) {
    const uint8_t* end = p + n; uint64_t h;
    if (n >= 32) {
        uint64_t v1 = P1 + P2, v2 = P2, v3 = 0, v4 = 0 - P1;
        do { v1 = xround(v1, le64(p)); v2 = xround(v2, le64(p + 8)); v3 = xround(v3, le64(p + 16)); v4 = xround(v4, le64(p + 24)); p += 32; } while ((size_t)(end - p) >= 32);
        h = rol64(v1, 1) + rol64(v2, 7) + rol64(v3, 12) + rol64(v4, 18);
        h = xmerge(h, v1); h = xmerge(h, v2); h = xmerge(h, v3); h = xmerge(h, v4);
    } else h = P5;
    h += n;
    while ((size_t)(end - p) >= 8) { h ^= xround(0, le64(p)); h = rol64(h, 27) * P1 + P4; p += 8; }
    if ((size_t)(end - p) >= 4) { h ^= (uint64_t)le32(p) * P1; h = rol64(h, 23) * P2 + P3; p += 4; }
    while (p < end) { h ^= (uint64_t)*p++ * P5; h = rol64(h, 11) * P1; }
    h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
    return h;
}

/* ---- поток битов ---- */
typedef struct { const uint8_t* p; size_t nbytes; int64_t avail; } bwd;       /* обратный: читается с конца, avail - непрочитанных бит */
static int hibit(unsigned v) { int r = -1; while (v) { v >>= 1; r++; } return r; }
static int bwd_init(bwd* b, const uint8_t* p, size_t n) {
    if (n == 0 || p[n - 1] == 0) return ZSTD_ERR_CORRUPT;                    /* в последнем байте должен быть маркер конца */
    b->p = p; b->nbytes = n; b->avail = (int64_t)(n - 1) * 8 + hibit(p[n - 1]);
    return 0;
}
static uint64_t bwd_peek(const bwd* b, int n) {                              /* n <= 32; за началом потока - нули */
    uint64_t v = 0;
    for (int i = 0; i < n; i++) {
        int64_t idx = b->avail - n + i;                                        /* номер бита от начала потока */
        if (idx >= 0) v |= (uint64_t)((b->p[idx >> 3] >> (idx & 7)) & 1) << i;
    }
    return v;
}
static uint64_t bwd_read(bwd* b, int n) { uint64_t v = bwd_peek(b, n); b->avail -= n; return v; }

typedef struct { const uint8_t* p; size_t nbytes; size_t bit; } fwd;          /* прямой: младшие биты первыми (заголовки FSE) */
static uint32_t fwd_peek(const fwd* f, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) { size_t idx = f->bit + (size_t)i; if ((idx >> 3) < f->nbytes) v |= (uint32_t)((f->p[idx >> 3] >> (idx & 7)) & 1) << i; }
    return v;
}

/* ---- FSE ---- */
typedef struct { uint16_t base; uint8_t sym, nbits; } fse_ent;
typedef struct { fse_ent t[512]; int al; int valid; } fse_tab;

static int fse_build(fse_tab* t, const int16_t* norm, int nsym, int al) {
    if (al > 9) return ZSTD_ERR_CORRUPT;
    int size = 1 << al, high = size - 1;
    uint16_t next[256];
    int total = 0;
    for (int s = 0; s < nsym; s++) {
        if (norm[s] == -1) { t->t[high--].sym = (uint8_t)s; next[s] = 1; total += 1; }
        else if (norm[s] >= 0) { next[s] = (uint16_t)norm[s]; total += norm[s]; }
        else return ZSTD_ERR_CORRUPT;
    }
    if (total != size) return ZSTD_ERR_CORRUPT;
    int step = (size >> 1) + (size >> 3) + 3, mask = size - 1, pos = 0;
    for (int s = 0; s < nsym; s++) for (int i = 0; i < norm[s]; i++) {
        t->t[pos].sym = (uint8_t)s;
        do pos = (pos + step) & mask; while (pos > high);
    }
    if (pos != 0) return ZSTD_ERR_CORRUPT;
    for (int u = 0; u < size; u++) {
        int s = t->t[u].sym; int nx = next[s]++;
        int nb = al - hibit((unsigned)nx);
        t->t[u].nbits = (uint8_t)nb; t->t[u].base = (uint16_t)((nx << nb) - size);
    }
    t->al = al; t->valid = 1;
    return 0;
}
/* Описание распределения: прочитать счётчики, построить таблицу. maxsym - наибольшее допустимое число символов, maxal - предел точности. Возвращает израсходованные байты или ошибку. */
static int fse_read_table(fse_tab* t, const uint8_t* p, size_t n, int maxsym, int maxal) {
    fwd f = { p, n, 0 };
    int al = 5 + (int)fwd_peek(&f, 4); f.bit += 4;
    if (al > maxal) return ZSTD_ERR_CORRUPT;
    int16_t norm[256]; for (int i = 0; i < 256; i++) norm[i] = 0;
    int remaining = (1 << al) + 1, threshold = 1 << al, nb = al + 1, sym = 0;
    while (remaining > 1 && sym < maxsym) {
        int max = (2 * threshold - 1) - remaining, count;
        uint32_t low = fwd_peek(&f, nb);
        if ((int)(low & (uint32_t)(threshold - 1)) < max) { count = (int)(low & (uint32_t)(threshold - 1)); f.bit += (size_t)(nb - 1); }
        else { count = (int)(low & (uint32_t)(2 * threshold - 1)); if (count >= threshold) count -= max; f.bit += (size_t)nb; }
        count--;
        remaining -= count < 0 ? -count : count;
        if (remaining < 1) return ZSTD_ERR_CORRUPT;
        norm[sym++] = (int16_t)count;
        if (count == 0) {                                                   /* повтор нулевых вероятностей: 2 бита, код 3 - прочитать ещё */
            int r;
            do {
                r = (int)fwd_peek(&f, 2); f.bit += 2;
                if (sym + r > maxsym) return ZSTD_ERR_CORRUPT;
                for (int k = 0; k < r; k++) norm[sym++] = 0;
            } while (r == 3);
        }
        while (remaining < threshold) { nb--; threshold >>= 1; }
    }
    if (remaining != 1 || (f.bit + 7) / 8 > n) return ZSTD_ERR_CORRUPT;
    int rc = fse_build(t, norm, sym, al);
    if (rc) return rc;
    return (int)((f.bit + 7) / 8);
}

/* стандартные распределения (RFC 8878, 3.1.1.3.2.2) */
static const int16_t LL_DEF[36] = {4,3,2,2,2,2,2,2,2,2,2,2,2,1,1,1,2,2,2,2,2,2,2,2,2,3,2,1,1,1,1,1,-1,-1,-1,-1};
static const int16_t ML_DEF[53] = {1,4,3,2,2,2,2,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,-1,-1,-1,-1,-1,-1,-1};
static const int16_t OF_DEF[29] = {1,1,1,1,1,1,2,2,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,-1,-1,-1,-1};
static const uint32_t LL_BASE[36] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,18,20,22,24,28,32,40,48,64,128,256,512,1024,2048,4096,8192,16384,32768,65536};
static const uint8_t LL_BITS[36] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,2,2,3,3,4,6,7,8,9,10,11,12,13,14,15,16};
static const uint32_t ML_BASE[53] = {3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,37,39,41,43,47,51,59,67,83,99,131,259,515,1027,2051,4099,8195,16387,32771,65539};
static const uint8_t ML_BITS[53] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,2,2,3,3,4,4,5,7,8,9,10,11,12,13,14,15,16};

/* ---- Huffman ---- */
typedef struct { uint8_t sym, nbits; } huf_ent;
typedef struct { huf_ent t[2048]; int maxbits; int valid; } huf_tab;

struct zstd_work {
    fse_tab ll, of, ml;
    huf_tab huf;
    uint8_t lit[ZSTD_BLOCK_MAX + 32];
    uint32_t rep[3];
};
size_t zstd_work_size(void) { return sizeof(struct zstd_work); }

/* Описание дерева: weights -> таблица. Возвращает число израсходованных байт описания. */
static int huf_read_tree(huf_tab* h, const uint8_t* p, size_t n) {
    if (n < 1) return ZSTD_ERR_TRUNC;
    uint8_t w[256]; int nw; size_t used;
    int hb = p[0];
    if (hb >= 128) {                                                         /* веса напрямую, по 4 бита */
        nw = hb - 127; used = 1 + (size_t)(nw + 1) / 2;
        if (used > n) return ZSTD_ERR_TRUNC;
        for (int i = 0; i < nw; i++) w[i] = (i & 1) ? (p[1 + i / 2] & 15) : (p[1 + i / 2] >> 4);
    } else {                                                                 /* веса сжаты FSE: два чередующихся состояния */
        used = 1 + (size_t)hb;
        if (hb == 0 || used > n) return ZSTD_ERR_CORRUPT;
        fse_tab ft;
        int hl = fse_read_table(&ft, p + 1, (size_t)hb, 13, 6);
        if (hl < 0) return hl;
        bwd b; int rc = bwd_init(&b, p + 1 + hl, (size_t)hb - (size_t)hl);
        if (rc) return rc;
        uint32_t s1 = (uint32_t)bwd_read(&b, ft.al), s2 = (uint32_t)bwd_read(&b, ft.al);
        if (b.avail < 0) return ZSTD_ERR_CORRUPT;
        nw = 0;
        for (;;) {
            if (nw >= 255) return ZSTD_ERR_CORRUPT;
            w[nw++] = ft.t[s1].sym; s1 = ft.t[s1].base + (uint32_t)bwd_read(&b, ft.t[s1].nbits);
            if (b.avail < 0) { if (nw >= 255) return ZSTD_ERR_CORRUPT; w[nw++] = ft.t[s2].sym; break; }
            if (nw >= 255) return ZSTD_ERR_CORRUPT;
            w[nw++] = ft.t[s2].sym; s2 = ft.t[s2].base + (uint32_t)bwd_read(&b, ft.t[s2].nbits);
            if (b.avail < 0) { if (nw >= 255) return ZSTD_ERR_CORRUPT; w[nw++] = ft.t[s1].sym; break; }
        }
    }
    uint32_t sum = 0;
    for (int i = 0; i < nw; i++) { if (w[i] > 11) return ZSTD_ERR_CORRUPT; if (w[i]) sum += 1u << (w[i] - 1); }
    if (sum == 0) return ZSTD_ERR_CORRUPT;
    int mb = hibit(sum) + 1;                                                 /* наименьшая степень двойки строго больше суммы */
    if (mb > 11) return ZSTD_ERR_CORRUPT;
    uint32_t rest = (1u << mb) - sum;                                        /* вклад последнего символа: степень двойки */
    if (rest == 0 || (rest & (rest - 1))) return ZSTD_ERR_CORRUPT;
    w[nw] = (uint8_t)(hibit(rest) + 1); nw++;
    h->maxbits = mb;
    uint32_t pos = 0;
    for (int wt = 1; wt <= mb; wt++) for (int s = 0; s < nw; s++) if (w[s] == wt) {
        uint32_t len = 1u << (wt - 1);
        for (uint32_t k = 0; k < len; k++) { h->t[pos + k].sym = (uint8_t)s; h->t[pos + k].nbits = (uint8_t)(mb + 1 - wt); }
        pos += len;
    }
    if (pos != (1u << mb)) return ZSTD_ERR_CORRUPT;
    h->valid = 1;
    return (int)used;
}
static int huf_decode(const huf_tab* h, const uint8_t* p, size_t n, uint8_t* out, size_t cnt) {
    bwd b; int rc = bwd_init(&b, p, n);
    if (rc) return rc;
    for (size_t i = 0; i < cnt; i++) {
        const huf_ent* e = &h->t[bwd_peek(&b, h->maxbits)];
        out[i] = e->sym; b.avail -= e->nbits;
        if (b.avail < 0) return ZSTD_ERR_CORRUPT;
    }
    return b.avail == 0 ? 0 : ZSTD_ERR_CORRUPT;
}

/* ---- блок ---- */
/* Секция литералов -> w->lit; возвращает израсходованные байты, *nlit - число литералов. */
static int read_literals(zstd_work* w, const uint8_t* p, size_t n, size_t* nlit) {
    if (n < 1) return ZSTD_ERR_TRUNC;
    int type = p[0] & 3, sf = (p[0] >> 2) & 3;
    size_t hdr, regen, comp = 0;
    if (type <= 1) {
        if (sf == 0 || sf == 2) { hdr = 1; regen = p[0] >> 3; }
        else if (sf == 1) { if (n < 2) return ZSTD_ERR_TRUNC; hdr = 2; regen = (size_t)(p[0] >> 4) | (size_t)p[1] << 4; }
        else { if (n < 3) return ZSTD_ERR_TRUNC; hdr = 3; regen = (size_t)(p[0] >> 4) | (size_t)p[1] << 4 | (size_t)p[2] << 12; }
        if (regen > ZSTD_BLOCK_MAX) return ZSTD_ERR_CORRUPT;
        if (type == 0) { if (n - hdr < regen) return ZSTD_ERR_TRUNC; for (size_t i = 0; i < regen; i++) w->lit[i] = p[hdr + i]; *nlit = regen; return (int)(hdr + regen); }
        if (n - hdr < 1) return ZSTD_ERR_TRUNC;
        for (size_t i = 0; i < regen; i++) w->lit[i] = p[hdr];
        *nlit = regen; return (int)(hdr + 1);
    }
    int streams;
    if (sf <= 1) { if (n < 3) return ZSTD_ERR_TRUNC; hdr = 3; uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; regen = (v >> 4) & 0x3FF; comp = (v >> 14) & 0x3FF; streams = sf == 0 ? 1 : 4; }
    else if (sf == 2) { if (n < 4) return ZSTD_ERR_TRUNC; hdr = 4; uint32_t v = le32(p); regen = (v >> 4) & 0x3FFF; comp = (v >> 18) & 0x3FFF; streams = 4; }
    else { if (n < 5) return ZSTD_ERR_TRUNC; hdr = 5; uint64_t v = (uint64_t)le32(p) | (uint64_t)p[4] << 32; regen = (size_t)((v >> 4) & 0x3FFFF); comp = (size_t)((v >> 22) & 0x3FFFF); streams = 4; }
    if (regen > ZSTD_BLOCK_MAX || comp > n - hdr) return n - hdr < comp ? ZSTD_ERR_TRUNC : ZSTD_ERR_CORRUPT;
    const uint8_t* body = p + hdr; size_t blen = comp;
    if (type == 2) {
        int used = huf_read_tree(&w->huf, body, blen);
        if (used < 0) return used;
        body += used; blen -= (size_t)used;
    } else if (!w->huf.valid) return ZSTD_ERR_CORRUPT;                       /* без дерева: нужно дерево предыдущего блока */
    if (streams == 1) {
        int rc = huf_decode(&w->huf, body, blen, w->lit, regen);
        if (rc) return rc;
    } else {
        if (blen < 6) return ZSTD_ERR_TRUNC;
        size_t s1 = (size_t)body[0] | (size_t)body[1] << 8, s2 = (size_t)body[2] | (size_t)body[3] << 8, s3 = (size_t)body[4] | (size_t)body[5] << 8;
        if (s1 + s2 + s3 + 6 >= blen) return ZSTD_ERR_CORRUPT;
        size_t s4 = blen - 6 - s1 - s2 - s3, q = (regen + 3) / 4;
        if (q * 3 > regen) return ZSTD_ERR_CORRUPT;
        const uint8_t* sp = body + 6; int rc;
        if ((rc = huf_decode(&w->huf, sp, s1, w->lit, q))) return rc;
        sp += s1;
        if ((rc = huf_decode(&w->huf, sp, s2, w->lit + q, q))) return rc;
        sp += s2;
        if ((rc = huf_decode(&w->huf, sp, s3, w->lit + 2 * q, q))) return rc;
        sp += s3;
        if ((rc = huf_decode(&w->huf, sp, s4, w->lit + 3 * q, regen - 3 * q))) return rc;
    }
    *nlit = regen;
    return (int)(hdr + comp);
}

/* Таблица одного вида кодов по режиму (0 стандартная, 1 один символ, 2 из потока, 3 прежняя). Возвращает израсходованные байты. */
static int seq_table(fse_tab* t, int mode, const uint8_t* p, size_t n, const int16_t* def, int ndef, int defal, int maxsym, int maxal) {
    if (mode == 0) { int rc = fse_build(t, def, ndef, defal); return rc ? rc : 0; }
    if (mode == 1) {
        if (n < 1) return ZSTD_ERR_TRUNC;
        if (p[0] >= maxsym) return ZSTD_ERR_CORRUPT;
        t->t[0].sym = p[0]; t->t[0].nbits = 0; t->t[0].base = 0; t->al = 0; t->valid = 1;
        return 1;
    }
    if (mode == 2) return fse_read_table(t, p, n, maxsym, maxal);
    return t->valid ? 0 : ZSTD_ERR_CORRUPT;                                  /* повтор таблицы предыдущего блока */
}

static int decode_block(zstd_work* w, const uint8_t* p, size_t n, uint8_t* dst, size_t pos, size_t cap, size_t* newpos) {
    size_t nlit = 0; int used = read_literals(w, p, n, &nlit);
    if (used < 0) return used;
    p += used; n -= (size_t)used;
    if (n < 1) return ZSTD_ERR_TRUNC;
    size_t nseq;
    if (p[0] < 128) { nseq = p[0]; p += 1; n -= 1; }
    else if (p[0] < 255) { if (n < 2) return ZSTD_ERR_TRUNC; nseq = ((size_t)(p[0] - 128) << 8) + p[1]; p += 2; n -= 2; }
    else { if (n < 3) return ZSTD_ERR_TRUNC; nseq = (size_t)p[1] + ((size_t)p[2] << 8) + 0x7F00; p += 3; n -= 3; }
    size_t li = 0, op = pos;
    if (nseq == 0) {
        if (n != 0) return ZSTD_ERR_CORRUPT;
        if (cap - op < nlit) return ZSTD_ERR_DST_SMALL;
        for (size_t i = 0; i < nlit; i++) dst[op++] = w->lit[i];
        *newpos = op; return 0;
    }
    if (n < 1) return ZSTD_ERR_TRUNC;
    int mll = p[0] >> 6, mof = (p[0] >> 4) & 3, mml = (p[0] >> 2) & 3;
    if (p[0] & 3) return ZSTD_ERR_CORRUPT;                                   /* зарезервированные биты */
    p += 1; n -= 1;
    int rc;
    if ((rc = seq_table(&w->ll, mll, p, n, LL_DEF, 36, 6, 36, 9)) < 0) return rc;
    p += rc; n -= (size_t)rc;
    if ((rc = seq_table(&w->of, mof, p, n, OF_DEF, 29, 5, 32, 8)) < 0) return rc;
    p += rc; n -= (size_t)rc;
    if ((rc = seq_table(&w->ml, mml, p, n, ML_DEF, 53, 6, 53, 9)) < 0) return rc;
    p += rc; n -= (size_t)rc;
    bwd b; if ((rc = bwd_init(&b, p, n))) return rc;
    uint32_t sl = (uint32_t)bwd_read(&b, w->ll.al), so = (uint32_t)bwd_read(&b, w->of.al), sm = (uint32_t)bwd_read(&b, w->ml.al);
    if (b.avail < 0) return ZSTD_ERR_CORRUPT;
    for (size_t i = 0; i < nseq; i++) {
        int occ = w->of.t[so].sym, mcc = w->ml.t[sm].sym, lcc = w->ll.t[sl].sym;
        if (lcc > 35 || mcc > 52 || occ > 31) return ZSTD_ERR_CORRUPT;
        uint64_t ofv = ((uint64_t)1 << occ) + bwd_read(&b, occ);
        uint32_t ml = ML_BASE[mcc] + (uint32_t)bwd_read(&b, ML_BITS[mcc]);
        uint32_t ll = LL_BASE[lcc] + (uint32_t)bwd_read(&b, LL_BITS[lcc]);
        if (b.avail < 0) return ZSTD_ERR_CORRUPT;
        uint64_t off;
        if (ofv > 3) { off = ofv - 3; w->rep[2] = w->rep[1]; w->rep[1] = w->rep[0]; w->rep[0] = (uint32_t)off; }
        else {
            unsigned idx = (unsigned)ofv - 1 + (ll == 0 ? 1u : 0u);          /* при пустых литералах номера сдвинуты на единицу */
            if (idx == 0) off = w->rep[0];
            else {
                if (idx == 3) { off = (uint64_t)w->rep[0] - 1; if (off == 0) return ZSTD_ERR_CORRUPT; }
                else off = w->rep[idx];
                if (idx >= 2) w->rep[2] = w->rep[1];
                w->rep[1] = w->rep[0]; w->rep[0] = (uint32_t)off;
            }
        }
        if (ofv > 0xFFFFFFFFu || off == 0) return ZSTD_ERR_CORRUPT;
        if (ll > nlit - li) return ZSTD_ERR_CORRUPT;
        if (cap - op < (size_t)ll + ml) return ZSTD_ERR_DST_SMALL;
        for (uint32_t k = 0; k < ll; k++) dst[op++] = w->lit[li++];
        if (off > op) return ZSTD_ERR_CORRUPT;                               /* смещение дальше начала выхода (словарей нет) */
        for (uint32_t k = 0; k < ml; k++) { dst[op] = dst[op - off]; op++; }
        if (i + 1 < nseq) {                                                  /* обновление состояний: литералы, длина совпадения, смещение */
            sl = w->ll.t[sl].base + (uint32_t)bwd_read(&b, w->ll.t[sl].nbits);
            sm = w->ml.t[sm].base + (uint32_t)bwd_read(&b, w->ml.t[sm].nbits);
            so = w->of.t[so].base + (uint32_t)bwd_read(&b, w->of.t[so].nbits);
            if (b.avail < 0) return ZSTD_ERR_CORRUPT;
        }
    }
    if (b.avail != 0) return ZSTD_ERR_CORRUPT;                               /* поток должен быть израсходован ровно */
    size_t rest = nlit - li;
    if (cap - op < rest) return ZSTD_ERR_DST_SMALL;
    for (size_t k = 0; k < rest; k++) dst[op++] = w->lit[li++];
    *newpos = op;
    return 0;
}

/* ---- кадры ---- */
static int frame_header(const uint8_t* p, size_t n, size_t* hdr, int64_t* content, int* checksum) {
    if (n < 5) return ZSTD_ERR_TRUNC;
    uint8_t fhd = p[4];
    int fcsf = fhd >> 6, single = (fhd >> 5) & 1, dictf = fhd & 3;
    if (fhd & 8) return ZSTD_ERR_CORRUPT;                                    /* зарезервированный бит */
    size_t h = 5 + (single ? 0 : 1);
    static const int dsz[4] = {0, 1, 2, 4};
    h += (size_t)dsz[dictf];
    int fcs = fcsf == 0 ? (single ? 1 : 0) : (1 << fcsf);
    h += (size_t)fcs;
    if (n < h) return ZSTD_ERR_TRUNC;
    size_t q = 5 + (single ? 0 : 1);
    if (dictf) { uint32_t id = 0; for (int i = 0; i < dsz[dictf]; i++) id |= (uint32_t)p[q + (size_t)i] << (8 * i); if (id) return ZSTD_ERR_UNSUPPORTED; }
    q += (size_t)dsz[dictf];
    int64_t c = -1;
    if (fcs) {
        uint64_t v = 0; for (int i = fcs - 1; i >= 0; i--) v = v << 8 | p[q + (size_t)i];
        if (fcs == 2) v += 256;
        if (v > (uint64_t)0x7FFFFFFFFFFFFFFFLL) return ZSTD_ERR_CORRUPT;
        c = (int64_t)v;
    }
    *hdr = h; *content = c; *checksum = (fhd >> 2) & 1;
    return 0;
}

int64_t zstd_frame_content_size(const void* src, size_t n) {
    const uint8_t* p = src; size_t h; int64_t c; int ck;
    if (n < 4 || le32(p) != 0xFD2FB528u) return -1;
    if (frame_header(p, n, &h, &c, &ck)) return -1;
    return c;
}

int zstd_decompress(const void* src, size_t n, void* dstv, size_t cap, size_t* out, zstd_work* w) {
    const uint8_t* p = src; uint8_t* dst = dstv; size_t pos = 0;
    if (!src || !dst || !w || !out) return ZSTD_ERR_FORMAT;
    if (n == 0) return ZSTD_ERR_TRUNC;
    while (n > 0) {
        if (n < 4) return ZSTD_ERR_TRUNC;
        uint32_t magic = le32(p);
        if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) {                          /* пропускаемый кадр */
            if (n < 8) return ZSTD_ERR_TRUNC;
            uint32_t sz = le32(p + 4);
            if (n - 8 < sz) return ZSTD_ERR_TRUNC;
            p += 8 + (size_t)sz; n -= 8 + (size_t)sz; continue;
        }
        if (magic != 0xFD2FB528u) return ZSTD_ERR_FORMAT;
        size_t hdr; int64_t content; int checksum;
        int rc = frame_header(p, n, &hdr, &content, &checksum);
        if (rc) return rc;
        p += hdr; n -= hdr;
        size_t fstart = pos;
        w->huf.valid = 0; w->ll.valid = w->of.valid = w->ml.valid = 0;
        w->rep[0] = 1; w->rep[1] = 4; w->rep[2] = 8;
        for (;;) {
            if (n < 3) return ZSTD_ERR_TRUNC;
            uint32_t bh = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
            int last = bh & 1, type = (bh >> 1) & 3; size_t bs = bh >> 3;
            p += 3; n -= 3;
            if (type == 3) return ZSTD_ERR_CORRUPT;
            if (type == 1) {
                if (n < 1) return ZSTD_ERR_TRUNC;
                if (bs > ZSTD_BLOCK_MAX) return ZSTD_ERR_CORRUPT;
                if (cap - pos < bs) return ZSTD_ERR_DST_SMALL;
                for (size_t i = 0; i < bs; i++) dst[pos + i] = p[0];
                pos += bs; p += 1; n -= 1;
            } else {
                if (bs > ZSTD_BLOCK_MAX) return ZSTD_ERR_CORRUPT;
                if (n < bs) return ZSTD_ERR_TRUNC;
                if (type == 0) { if (cap - pos < bs) return ZSTD_ERR_DST_SMALL; for (size_t i = 0; i < bs; i++) dst[pos + i] = p[i]; pos += bs; }
                else { rc = decode_block(w, p, bs, dst, pos, cap, &pos); if (rc) return rc; }
                p += bs; n -= bs;
            }
            if (last) break;
        }
        if (content >= 0 && (uint64_t)(pos - fstart) != (uint64_t)content) return ZSTD_ERR_CORRUPT;
        if (checksum) {
            if (n < 4) return ZSTD_ERR_TRUNC;
            if (le32(p) != (uint32_t)xxh64(dst + fstart, pos - fstart)) return ZSTD_ERR_CHECKSUM;
            p += 4; n -= 4;
        }
    }
    *out = pos;
    return ZSTD_OK;
}
