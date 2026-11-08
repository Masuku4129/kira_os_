/* kcc: программная арифметика IEEE 754 (binary32 и binary64) на одних целых числах.
 * Нужна компилятору, чтобы разбирать литералы и сворачивать константные выражения без единой команды SSE/FPU:
 * сам kcc собирается с -mno-sse (он работает на Кире, где ядро SSE не использует), поэтому типов float/double в его коде нет.
 * Значения - битовые образы (binary32 лежит в младших 32 битах). Режим округления - к ближайшему чётному, как у кода, который выдаёт бэкенд.
 * Исключения (inexact, overflow...) не отслеживаются. NaN на выходе - тихий положительный 0x7FC00000 / 0x7FF8000000000000, как при свёртке в clang. */
#include "kcc.h"
#include <string.h>

typedef struct { int sign, kind, e; uint64_t m; } SfV;   /* kind: 0 ноль, 1 конечное, 2 бесконечность, 3 NaN; значение = m * 2^(e-63), старший бит m установлен */

static int sf_eb(int sz) { return sz == 4 ? 8 : 11; }
static int sf_fb(int sz) { return sz == 4 ? 23 : 52; }
static int sf_bias(int sz) { return sz == 4 ? 127 : 1023; }
static uint64_t sf_nan(int sz) { return sz == 4 ? 0x7FC00000ULL : 0x7FF8000000000000ULL; }

static int sf_bitlen64(uint64_t v) {
    int n = 0;
    while (v) { n++; v >>= 1; }
    return n;
}

static SfV sf_unpack(uint64_t bits, int sz) {
    SfV r;
    int fb = sf_fb(sz), eb = sf_eb(sz);
    uint64_t frac = bits & ((1ULL << fb) - 1);
    int ex = (int)((bits >> fb) & ((1ULL << eb) - 1));
    r.sign = (int)((bits >> (fb + eb)) & 1);
    r.e = 0; r.m = 0;
    if (ex == (1 << eb) - 1) { r.kind = frac ? 3 : 2; return r; }
    if (ex == 0) {
        if (!frac) { r.kind = 0; return r; }
        int h = sf_bitlen64(frac) - 1;
        r.kind = 1; r.m = frac << (63 - h); r.e = h + 1 - sf_bias(sz) - fb;
        return r;
    }
    r.kind = 1; r.m = (frac | (1ULL << fb)) << (63 - fb); r.e = ex - sf_bias(sz);
    return r;
}

/* Округление к ближайшему чётному: значение (-1)^sign * m * 2^(e-63) (m нормализована) плюс sticky - ненулевой хвост ниже m. */
static uint64_t sf_pack(int sz, int sign, int e, uint64_t m, int sticky) {
    int fb = sf_fb(sz), eb = sf_eb(sz), bias = sf_bias(sz), P = fb + 1;
    uint64_t sg = (uint64_t)sign << (fb + eb);
    uint64_t inf = ((1ULL << eb) - 1) << fb;
    if (e > bias) return sg | inf;
    int emin = 1 - bias, drop = 64 - P;
    uint64_t base;
    if (e < emin) { drop += emin - e; base = 0; }
    else base = (uint64_t)(e + bias - 1);
    uint64_t q = 0;
    int up = 0;
    if (drop > 64) { q = 0; up = 0; }
    else if (drop == 64) { q = 0; up = m > (1ULL << 63) || (m == (1ULL << 63) && sticky); }
    else {
        q = m >> drop;
        uint64_t rem = m & ((1ULL << drop) - 1), half = 1ULL << (drop - 1);
        up = rem > half || (rem == half && (sticky || (q & 1)));
    }
    q += (uint64_t)up;
    uint64_t bits = (base << fb) + q;
    if (bits >= inf) return sg | inf;
    return sg | bits;
}

/* Значение S * 2^(e0) с произвольным 64-битным S: нормализация и упаковка. */
static uint64_t sf_pack_s(int sz, int sign, uint64_t s, int e0, int sticky) {
    if (!s) return sf_pack(sz, sign, -100000, 0, sticky);       /* только хвост: уходит в ноль */
    int p = sf_bitlen64(s) - 1;
    return sf_pack(sz, sign, e0 + p, s << (63 - p), sticky);
}

uint64_t sf_neg(uint64_t a, int sz) { return a ^ (1ULL << (sf_fb(sz) + sf_eb(sz))); }

uint64_t sf_binop(int op, uint64_t xa, uint64_t xb, int sz) {
    SfV a = sf_unpack(xa, sz), b = sf_unpack(xb, sz);
    uint64_t nan = sf_nan(sz), inf = ((1ULL << sf_eb(sz)) - 1) << sf_fb(sz);
    int sgn_shift = sf_fb(sz) + sf_eb(sz);
    if (a.kind == 3 || b.kind == 3) return nan;
    if (op == '-') b.sign ^= 1;
    if (op == '+' || op == '-') {
        if (a.kind == 2 || b.kind == 2) {
            if (a.kind == 2 && b.kind == 2 && a.sign != b.sign) return nan;
            return inf | ((uint64_t)(a.kind == 2 ? a.sign : b.sign) << sgn_shift);
        }
        if (a.kind == 0 && b.kind == 0) return (uint64_t)(a.sign & b.sign) << sgn_shift;
        if (b.kind == 0) return sf_pack(sz, a.sign, a.e, a.m, 0);
        if (a.kind == 0) return sf_pack(sz, b.sign, b.e, b.m, 0);
        if (a.e < b.e || (a.e == b.e && a.m < b.m)) { SfV t = a; a = b; b = t; }     /* |a| >= |b| */
        uint64_t ma = a.m >> 1, mb = b.m >> 1;
        int d = a.e - b.e, sticky = 0;
        if (d >= 64) { sticky = mb != 0; mb = 0; }
        else if (d > 0) { sticky = (mb & ((1ULL << d) - 1)) != 0; mb >>= d; }
        uint64_t s;
        if (a.sign == b.sign) s = ma + mb;
        else {
            s = ma - mb - (uint64_t)sticky;
            if (!s && !sticky) return 0;                         /* x - x = +0 */
        }
        return sf_pack_s(sz, a.sign, s, a.e - 62, sticky);
    }
    int sign = a.sign ^ b.sign;
    if (op == '*') {
        if (a.kind == 2 || b.kind == 2) {
            if (a.kind == 0 || b.kind == 0) return nan;
            return inf | ((uint64_t)sign << sgn_shift);
        }
        if (a.kind == 0 || b.kind == 0) return (uint64_t)sign << sgn_shift;
        uint64_t a0 = a.m & 0xffffffffULL, a1 = a.m >> 32, b0 = b.m & 0xffffffffULL, b1 = b.m >> 32;
        uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
        uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffULL) + (p10 & 0xffffffffULL);
        uint64_t lo = (p00 & 0xffffffffULL) | (mid << 32);
        uint64_t hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
        int e = a.e + b.e + 1;
        if (!(hi >> 63)) { hi = (hi << 1) | (lo >> 63); lo <<= 1; e--; }
        return sf_pack(sz, sign, e, hi, lo != 0);
    }
    /* деление */
    if (a.kind == 2) return b.kind == 2 ? nan : (inf | ((uint64_t)sign << sgn_shift));
    if (b.kind == 2) return (uint64_t)sign << sgn_shift;
    if (b.kind == 0) return a.kind == 0 ? nan : (inf | ((uint64_t)sign << sgn_shift));
    if (a.kind == 0) return (uint64_t)sign << sgn_shift;
    uint64_t r = a.m, q = 0;
    int e = a.e - b.e, rc = 0;
    if (a.m < b.m) { rc = 1; r = a.m << 1; e--; }
    r -= b.m; q = 1;                                              /* первая цифра: значение rc:r >= b.m */
    for (int i = 0; i < 63; i++) {
        rc = (int)(r >> 63); r <<= 1;
        if (rc || r >= b.m) { r -= b.m; q = (q << 1) | 1; }
        else q <<= 1;
    }
    return sf_pack(sz, sign, e, q, r != 0);
}

/* -1, 0, 1 - порядок; 2 - не упорядочены (NaN) */
int sf_cmp(uint64_t a, uint64_t b, int sz) {
    SfV x = sf_unpack(a, sz), y = sf_unpack(b, sz);
    if (x.kind == 3 || y.kind == 3) return 2;
    int sbit = sf_fb(sz) + sf_eb(sz);
    uint64_t mask = (1ULL << sbit) - 1;
    int64_t ka = (int64_t)(a & mask), kb = (int64_t)(b & mask);
    if ((a >> sbit) & 1) ka = -ka;
    if ((b >> sbit) & 1) kb = -kb;
    return ka < kb ? -1 : ka > kb ? 1 : 0;
}

uint64_t sf_from_int(int64_t v, int is_unsigned, int sz) {
    if (!v) return 0;
    int sign = !is_unsigned && v < 0;
    uint64_t mag = sign ? 0 - (uint64_t)v : (uint64_t)v;
    return sf_pack_s(sz, sign, mag, 0, 0);
}

/* Усечение к нулю. Возвращает биты результата (для беззнакового назначения - значение uint64). Вне диапазона: насыщение; *ok = 0. */
int64_t sf_to_int(uint64_t a, int sz, int is_unsigned, int *ok) {
    SfV x = sf_unpack(a, sz);
    *ok = 1;
    if (x.kind == 3) { *ok = 0; return 0; }
    if (x.kind == 0) return 0;
    if (x.kind == 2) { *ok = 0; return x.sign ? (is_unsigned ? 0 : INT64_MIN) : (is_unsigned ? -1 : INT64_MAX); }
    if (x.e < 0) return 0;
    if (x.e >= 64 || (!is_unsigned && x.e == 63 && !(x.sign && x.m == (1ULL << 63))) || (is_unsigned && x.sign)) {
        *ok = 0;
        if (is_unsigned) return x.sign ? 0 : -1;
        return x.sign ? INT64_MIN : INT64_MAX;
    }
    uint64_t mag = x.m >> (63 - x.e);
    return x.sign ? (int64_t)(0 - mag) : (int64_t)mag;
}

uint64_t sf_conv(uint64_t a, int from, int to) {
    if (from == to) return a;
    SfV x = sf_unpack(a, from);
    int sbit = sf_fb(to) + sf_eb(to);
    if (x.kind == 3) return sf_nan(to);
    if (x.kind == 2) return (((1ULL << sf_eb(to)) - 1) << sf_fb(to)) | ((uint64_t)x.sign << sbit);
    if (x.kind == 0) return (uint64_t)x.sign << sbit;
    return sf_pack(to, x.sign, x.e, x.m, 0);
}

/* ---------- разбор литералов ---------- */
#define SF_NL 140                                 /* 4480 бит: хватает для 780 десятичных цифр и 10^350 */
typedef struct { uint32_t d[SF_NL]; int n; } SfBig;

static void sf_big_zero(SfBig *b) { b->n = 0; }
static int sf_big_isz(const SfBig *b) { return b->n == 0; }
static void sf_big_mul_add(SfBig *b, uint32_t m, uint32_t a) {
    uint64_t c = a;
    for (int i = 0; i < b->n; i++) {
        uint64_t t = (uint64_t)b->d[i] * m + c;
        b->d[i] = (uint32_t)t; c = t >> 32;
    }
    if (c) { if (b->n < SF_NL) b->d[b->n++] = (uint32_t)c; }
}
static int sf_big_bitlen(const SfBig *b) { return b->n ? (b->n - 1) * 32 + sf_bitlen64(b->d[b->n - 1]) : 0; }
static void sf_big_shl(SfBig *b, int k) {
    if (!b->n || k <= 0) return;
    int w = k / 32, s = k % 32;
    int nn = b->n + w + (s ? 1 : 0);
    if (nn > SF_NL) nn = SF_NL;
    for (int i = nn - 1; i >= 0; i--) {
        int src = i - w;
        uint64_t v = 0;
        if (src >= 0 && src < b->n) v = (uint64_t)b->d[src] << s;
        if (s && src - 1 >= 0 && src - 1 < b->n) v |= (uint64_t)b->d[src - 1] >> (32 - s);
        b->d[i] = (uint32_t)v;
    }
    b->n = nn;
    while (b->n && !b->d[b->n - 1]) b->n--;
}
static void sf_big_shr1(SfBig *b) {
    for (int i = 0; i < b->n; i++) b->d[i] = (b->d[i] >> 1) | (i + 1 < b->n ? b->d[i + 1] << 31 : 0);
    while (b->n && !b->d[b->n - 1]) b->n--;
}
static int sf_big_cmp(const SfBig *a, const SfBig *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--) if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}
static void sf_big_sub(SfBig *a, const SfBig *b) {      /* a -= b, a >= b */
    int64_t br = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t t = (int64_t)a->d[i] - (i < b->n ? (int64_t)b->d[i] : 0) - br;
        br = t < 0; if (t < 0) t += (int64_t)1 << 32;
        a->d[i] = (uint32_t)t;
    }
    while (a->n && !a->d[a->n - 1]) a->n--;
}
/* верхние 64 бита числа (нормализованные), sticky - остальное ненулевое; *bl - длина числа в битах */
static uint64_t sf_big_top64(const SfBig *b, int *sticky) {
    int bl = sf_big_bitlen(b);
    uint64_t m = 0;
    *sticky = 0;
    for (int k = 0; k < 64; k++) {
        int pos = bl - 1 - k;
        if (pos < 0) break;
        m |= (uint64_t)((b->d[pos / 32] >> (pos % 32)) & 1) << (63 - k);
    }
    if (bl > 64) {
        int low = bl - 64;
        for (int i = 0; i < low / 32 && !*sticky; i++) if (b->d[i]) *sticky = 1;
        if (!*sticky && (low % 32) && (b->d[low / 32] & ((1u << (low % 32)) - 1))) *sticky = 1;
    }
    return m;
}

static int sf_hexv(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Разбор вещественной константы s[0..len). *sz на входе - 8; суффикс f/F даёт 4. Возвращает биты; ошибки - через kerr_tok. */
uint64_t sf_parse(const Token *t, int *sz) {
    const char *p = t->s, *e = t->s + t->len;
    int hex = e - p > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X');
    *sz = 8;
    if (hex) p += 2;
    SfBig big, dv;
    sf_big_zero(&big);
    int ndig = 0, dexp = 0, seen_dot = 0, sticky_dig = 0, anydig = 0;
    uint64_t hm = 0;                                             /* hex: накопленная мантисса (до 64 бит) */
    int hexp = 0;
    for (; p < e; p++) {
        if (*p == '.') { if (seen_dot) kerr_tok(t, "invalid floating constant '%.*s'", t->len, t->s); seen_dot = 1; continue; }
        int d = hex ? sf_hexv(*p) : (*p >= '0' && *p <= '9' ? *p - '0' : -1);
        if (d < 0) break;
        anydig = 1;
        if (hex) {
            if (!(hm >> 60)) { hm = (hm << 4) | (uint64_t)d; if (seen_dot) hexp -= 4; }
            else { if (d) sticky_dig = 1; if (!seen_dot) hexp += 4; }
            continue;
        }
        if (ndig == 0 && d == 0) { if (seen_dot) dexp--; continue; }
        if (ndig >= 780) { if (d) sticky_dig = 1; if (!seen_dot) dexp++; continue; }
        sf_big_mul_add(&big, 10, (uint32_t)d); ndig++;
        if (seen_dot) dexp--;
    }
    if (!anydig) kerr_tok(t, "invalid floating constant '%.*s'", t->len, t->s);
    int ex10 = 0, exsign = 1, hasexp = 0;
    if (p < e && ((!hex && (*p == 'e' || *p == 'E')) || (hex && (*p == 'p' || *p == 'P')))) {
        hasexp = 1; p++;
        if (p < e && (*p == '+' || *p == '-')) { if (*p == '-') exsign = -1; p++; }
        if (p >= e || *p < '0' || *p > '9') kerr_tok(t, "exponent has no digits in '%.*s'", t->len, t->s);
        for (; p < e && *p >= '0' && *p <= '9'; p++) if (ex10 < 100000) ex10 = ex10 * 10 + (*p - '0');
    }
    if (hex && !hasexp) kerr_tok(t, "hexadecimal floating constant requires an exponent");
    if (p < e && (*p == 'f' || *p == 'F')) { *sz = 4; p++; }
    else if (p < e && (*p == 'l' || *p == 'L')) kerr_tok(t, "unsupported: long double constant '%.*s' (kcc has no 80-bit long double)", t->len, t->s);
    if (p != e) kerr_tok(t, "invalid suffix on floating constant '%.*s'", t->len, t->s);
    int fsz = *sz;
    if (hex) {
        if (!hm) return 0;
        int bl = sf_bitlen64(hm);
        uint64_t m = hm << (64 - bl);
        return sf_pack(fsz, 0, (bl - 1) + hexp + exsign * ex10, m, sticky_dig);
    }
    if (sf_big_isz(&big)) return 0;
    if (sticky_dig) { sf_big_mul_add(&big, 10, 1); dexp--; }
    int total = dexp + exsign * ex10;
    int mag = ndig + (sticky_dig ? 1 : 0) + total;               /* значение < 10^mag */
    if (mag > 330) return ((1ULL << sf_eb(fsz)) - 1) << sf_fb(fsz);
    if (mag < -350) return 0;
    if (total >= 0) {
        for (int i = 0; i < total; i++) sf_big_mul_add(&big, 10, 0);
        int st;
        uint64_t m = sf_big_top64(&big, &st);
        return sf_pack(fsz, 0, sf_big_bitlen(&big) - 1, m, st);
    }
    sf_big_zero(&dv);
    dv.d[0] = 1; dv.n = 1;
    for (int i = 0; i < -total; i++) sf_big_mul_add(&dv, 10, 0);
    int s = sf_big_bitlen(&dv) - sf_big_bitlen(&big) + 63, tsh = 0;
    if (s >= 0) sf_big_shl(&big, s);
    else { tsh = -s; sf_big_shl(&dv, tsh); s = 0; }
    SfBig tt = dv;
    sf_big_shl(&tt, 63);
    uint64_t q = 0;
    for (int i = 63; i >= 0; i--) {
        if (sf_big_cmp(&big, &tt) >= 0) { sf_big_sub(&big, &tt); q |= 1ULL << i; }
        sf_big_shr1(&tt);
    }
    return sf_pack_s(fsz, 0, q, tsh - s, !sf_big_isz(&big));
}
