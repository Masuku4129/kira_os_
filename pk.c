/* RSA и ECDSA: проверка подписей. Числа - младшими 32-битными частями вперёд, до 4096 бит. */
#include "pk.h"
#include "crypto.h"

#define BN 130                                   /* 4096 бит + запас */
typedef uint32_t bn_t[BN];

typedef struct { int L; uint32_t np; bn_t n, rr, one; } mont_t;  /* модуль, -n^-1 mod 2^32, R^2 mod n, R mod n */

static void bn_zero(uint32_t* a, int L) { for (int i = 0; i < L; i++) a[i] = 0; }
static void bn_copy(uint32_t* d, const uint32_t* s, int L) { for (int i = 0; i < L; i++) d[i] = s[i]; }
static int bn_cmp(const uint32_t* a, const uint32_t* b, int L) {
    for (int i = L - 1; i >= 0; i--) if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    return 0;
}
static int bn_is_zero(const uint32_t* a, int L) { for (int i = 0; i < L; i++) if (a[i]) return 0; return 1; }
static uint32_t bn_sub(uint32_t* r, const uint32_t* a, const uint32_t* b, int L) {
    uint64_t br = 0;
    for (int i = 0; i < L; i++) { uint64_t d = (uint64_t)a[i] - b[i] - br; r[i] = (uint32_t)d; br = (d >> 32) & 1; }
    return (uint32_t)br;
}
static uint32_t bn_add(uint32_t* r, const uint32_t* a, const uint32_t* b, int L) {
    uint64_t c = 0;
    for (int i = 0; i < L; i++) { c += (uint64_t)a[i] + b[i]; r[i] = (uint32_t)c; c >>= 32; }
    return (uint32_t)c;
}
/* Большой порядок байт -> число из L частей; 0 - влезло. */
static int bn_from(uint32_t* a, int L, const uint8_t* b, size_t n) {
    while (n && !*b) { b++; n--; }
    if (n > (size_t)L * 4) return -1;
    bn_zero(a, L);
    for (size_t i = 0; i < n; i++) a[i / 4] |= (uint32_t)b[n - 1 - i] << (8 * (i % 4));
    return 0;
}
static void bn_to(uint8_t* b, size_t n, const uint32_t* a) {
    for (size_t i = 0; i < n; i++) b[n - 1 - i] = (uint8_t)(a[i / 4] >> (8 * (i % 4)));
}
static int bn_bits(const uint32_t* a, int L) {
    for (int i = L - 1; i >= 0; i--) if (a[i]) { int b = 32; while (!(a[i] >> (b - 1))) b--; return i * 32 + b; }
    return 0;
}

static void mont_mul(const mont_t* m, uint32_t* r, const uint32_t* a, const uint32_t* b) {
    uint32_t t[BN + 2];
    int L = m->L;
    for (int i = 0; i < L + 2; i++) t[i] = 0;
    for (int i = 0; i < L; i++) {
        uint64_t c = 0;
        for (int j = 0; j < L; j++) { c += (uint64_t)t[j] + (uint64_t)a[j] * b[i]; t[j] = (uint32_t)c; c >>= 32; }
        c += t[L]; t[L] = (uint32_t)c; t[L + 1] = (uint32_t)(c >> 32);
        uint32_t mm = t[0] * m->np;
        c = ((uint64_t)mm * m->n[0] + t[0]) >> 32;
        for (int j = 1; j < L; j++) { c += (uint64_t)mm * m->n[j] + t[j]; t[j - 1] = (uint32_t)c; c >>= 32; }
        c += t[L]; t[L - 1] = (uint32_t)c; c >>= 32;
        t[L] = t[L + 1] + (uint32_t)c;
    }
    if (t[L] || bn_cmp(t, m->n, L) >= 0) bn_sub(t, t, m->n, L);
    bn_copy(r, t, L);
}

static int mont_init(mont_t* m, const uint32_t* n, int L) {
    if (!(n[0] & 1) || L > BN - 2) return -1;
    m->L = L; bn_copy(m->n, n, L);
    uint32_t x = n[0];
    for (int i = 0; i < 5; i++) x *= 2 - n[0] * x;               /* обратное по модулю 2^32 (Ньютон) */
    m->np = (uint32_t)0 - x;
    bn_zero(m->rr, L); m->rr[0] = 1;
    for (int i = 0; i < 64 * L; i++) {                           /* 2^(64L) mod n = R^2 */
        uint32_t c = bn_add(m->rr, m->rr, m->rr, L);
        if (c || bn_cmp(m->rr, n, L) >= 0) bn_sub(m->rr, m->rr, n, L);
    }
    bn_t o; bn_zero(o, L); o[0] = 1;
    mont_mul(m, m->one, o, m->rr);                               /* R mod n - единица в форме Монтгомери */
    return 0;
}
static void to_m(const mont_t* m, uint32_t* r, const uint32_t* a) { mont_mul(m, r, a, m->rr); }
static void from_m(const mont_t* m, uint32_t* r, const uint32_t* a) { bn_t o; bn_zero(o, m->L); o[0] = 1; mont_mul(m, r, a, o); }
/* r = a^e (a и r - в форме Монтгомери), e - число из eL частей. */
static void mont_pow(const mont_t* m, uint32_t* r, const uint32_t* a, const uint32_t* e, int eL) {
    bn_t acc; bn_copy(acc, m->one, m->L);
    for (int i = bn_bits(e, eL) - 1; i >= 0; i--) {
        mont_mul(m, acc, acc, acc);
        if ((e[i / 32] >> (i % 32)) & 1) mont_mul(m, acc, acc, a);
    }
    bn_copy(r, acc, m->L);
}

/* ---------------- хэши ---------------- */
void pk_hash(int alg, const void* d, size_t n, uint8_t* out) {
    if (alg == PK_SHA256) { sha256(d, n, out); return; }
    sha512_ctx_t c;
    if (alg == PK_SHA384) sha384_init(&c); else sha512_init(&c);
    sha512_update(&c, d, n); sha512_final(&c, out);
}

/* ---------------- RSA ---------------- */
static mont_t rm;
static int rsa_em(const uint8_t* n, size_t nlen, const uint8_t* e, size_t elen, const uint8_t* sig, size_t slen,
                  uint8_t* em, size_t* k, int* modbits) {
    while (nlen && !*n) { n++; nlen--; }
    if (nlen < 128 || nlen > 512 || slen != nlen) return -1;      /* 1024..4096 бит, подпись - длины модуля */
    int L = (int)((nlen + 3) / 4);
    bn_t N, S, E, x;
    if (bn_from(N, L, n, nlen) || bn_from(S, L, sig, slen) || elen > 8 || bn_from(E, 2, e, elen)) return -1;
    if (bn_cmp(S, N, L) >= 0 || bn_is_zero(E, 2) || mont_init(&rm, N, L)) return -1;
    to_m(&rm, x, S);
    mont_pow(&rm, x, x, E, 2);
    from_m(&rm, x, x);
    *k = nlen; *modbits = bn_bits(N, L);
    bn_to(em, nlen, x);
    return 0;
}

static const uint8_t di256[] = { 0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20 };
static const uint8_t di384[] = { 0x30,0x41,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02,0x05,0x00,0x04,0x30 };
static const uint8_t di512[] = { 0x30,0x51,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x03,0x05,0x00,0x04,0x40 };

int rsa_verify_pkcs1(const uint8_t* n, size_t nlen, const uint8_t* e, size_t elen, int alg, const uint8_t* digest,
                     const uint8_t* sig, size_t slen) {
    static uint8_t em[512];
    size_t k; int mb;
    if (rsa_em(n, nlen, e, elen, sig, slen, em, &k, &mb)) return -1;
    const uint8_t* di = alg == PK_SHA256 ? di256 : alg == PK_SHA384 ? di384 : alg == PK_SHA512 ? di512 : 0;
    if (!di) return -1;
    size_t tl = 19 + (size_t)alg;
    if (k < tl + 11 || em[0] != 0 || em[1] != 1) return -1;
    size_t ps = k - tl - 3;
    for (size_t i = 0; i < ps; i++) if (em[2 + i] != 0xFF) return -1;
    if (em[2 + ps] != 0) return -1;
    if (!crypto_eq(em + 3 + ps, di, 19) || !crypto_eq(em + 3 + ps + 19, digest, (size_t)alg)) return -1;
    return 0;
}

/* RSASSA-PSS (RFC 8017 §9.1.2) с произвольными параметрами из сертификата: alg - хэш сообщения (digest этой длины),
   mgf_alg - хэш в MGF1, salt - длина соли в байтах. */
int rsa_verify_pss_ex(const uint8_t* n, size_t nlen, const uint8_t* e, size_t elen, int alg, int mgf_alg, size_t salt,
                      const uint8_t* digest, const uint8_t* sig, size_t slen) {
    static uint8_t em_full[512], db[512];
    size_t k; int mb;
    if (alg != PK_SHA256 && alg != PK_SHA384 && alg != PK_SHA512) return -1;
    if (mgf_alg != PK_SHA256 && mgf_alg != PK_SHA384 && mgf_alg != PK_SHA512) return -1;
    if (rsa_em(n, nlen, e, elen, sig, slen, em_full, &k, &mb)) return -1;
    size_t embits = (size_t)mb - 1, emlen = (embits + 7) / 8, h = (size_t)alg, mh = (size_t)mgf_alg;
    const uint8_t* em = em_full + (k - emlen);
    if (k > emlen && em_full[0]) return -1;
    if (emlen < h + salt + 2 || em[emlen - 1] != 0xbc) return -1;
    size_t dbl = emlen - h - 1;
    const uint8_t* H = em + dbl;
    unsigned top = (unsigned)(8 * emlen - embits);
    if (top && (em[0] >> (8 - top))) return -1;
    uint8_t cnt[64 + 4], blk[64];                                  /* MGF1: Hash(H || счётчик), H длиной h */
    for (size_t i = 0; i < h; i++) cnt[i] = H[i];
    for (uint32_t c = 0, done = 0; done < dbl; c++) {
        cnt[h] = (uint8_t)(c >> 24); cnt[h + 1] = (uint8_t)(c >> 16); cnt[h + 2] = (uint8_t)(c >> 8); cnt[h + 3] = (uint8_t)c;
        pk_hash(mgf_alg, cnt, h + 4, blk);
        for (size_t i = 0; i < mh && done < dbl; i++, done++) db[done] = em[done] ^ blk[i];
    }
    if (top) db[0] &= (uint8_t)(0xFF >> top);
    size_t z = dbl - salt - 1;
    for (size_t i = 0; i < z; i++) if (db[i]) return -1;
    if (db[z] != 1) return -1;
    uint8_t mp[8 + 64 + 512], hh[64];
    if (salt > 512) return -1;
    for (int i = 0; i < 8; i++) mp[i] = 0;
    for (size_t i = 0; i < h; i++) mp[8 + i] = digest[i];
    for (size_t i = 0; i < salt; i++) mp[8 + h + i] = db[z + 1 + i];
    pk_hash(alg, mp, 8 + h + salt, hh);
    return crypto_eq(hh, H, h) ? 0 : -1;
}
int rsa_verify_pss(const uint8_t* n, size_t nlen, const uint8_t* e, size_t elen, int alg, const uint8_t* digest,
                   const uint8_t* sig, size_t slen) {
    return rsa_verify_pss_ex(n, nlen, e, elen, alg, alg, (size_t)alg, digest, sig, slen);
}

/* ---------------- ECDSA: кривые NIST с a = -3 ---------------- */
typedef struct { int L; const char *p, *n, *b, *gx, *gy; } curve_def_t;
static const curve_def_t P256 = { 8,
    "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
    "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
    "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
    "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
    "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5" };
static const curve_def_t P384 = { 12,
    "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
    "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
    "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
    "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
    "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f" };

static void hexnum(uint32_t* a, int L, const char* h) {
    uint8_t b[48]; size_t n = 0;
    for (; h[0] && h[1] && n < sizeof b; h += 2) {
        int v = 0;
        for (int k = 0; k < 2; k++) { char c = h[k]; v = v * 16 + (c <= '9' ? c - '0' : c - 'a' + 10); }
        b[n++] = (uint8_t)v;
    }
    (void)bn_from(a, L, b, n);
}

typedef struct { bn_t x, y, z; } jpt_t;                         /* Якоби, в форме Монтгомери по p; z = 0 - бесконечность */
static mont_t fp, fn;
static bn_t c_b, c_three;
static int L_;

static void fadd(uint32_t* r, const uint32_t* a, const uint32_t* b) { if (bn_add(r, a, b, L_) || bn_cmp(r, fp.n, L_) >= 0) bn_sub(r, r, fp.n, L_); }
static void fsub(uint32_t* r, const uint32_t* a, const uint32_t* b) { if (bn_sub(r, a, b, L_)) bn_add(r, r, fp.n, L_); }
static void fmul(uint32_t* r, const uint32_t* a, const uint32_t* b) { mont_mul(&fp, r, a, b); }

static void pdbl(jpt_t* r, const jpt_t* p) {
    if (bn_is_zero(p->z, L_)) { *r = *p; return; }
    bn_t d, g, be, al, t1, t2, x3, y3, z3;
    fmul(d, p->z, p->z); fmul(g, p->y, p->y); fmul(be, p->x, g);
    fsub(t1, p->x, d); fadd(t2, p->x, d); fmul(al, t1, t2); fadd(t1, al, al); fadd(al, t1, al);       /* 3(X-d)(X+d) */
    fmul(x3, al, al); fadd(t1, be, be); fadd(t1, t1, t1); fadd(t2, t1, t1); fsub(x3, x3, t2);           /* - 8 beta */
    fadd(z3, p->y, p->z); fmul(z3, z3, z3); fsub(z3, z3, g); fsub(z3, z3, d);
    fsub(t1, t1, x3); fmul(y3, al, t1); fmul(t2, g, g); fadd(t2, t2, t2); fadd(t2, t2, t2); fadd(t2, t2, t2); fsub(y3, y3, t2);
    bn_copy(r->x, x3, L_); bn_copy(r->y, y3, L_); bn_copy(r->z, z3, L_);
}
static void padd(jpt_t* r, const jpt_t* p, const jpt_t* q) {
    if (bn_is_zero(p->z, L_)) { *r = *q; return; }
    if (bn_is_zero(q->z, L_)) { *r = *p; return; }
    bn_t z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, t, x3, y3, z3;
    fmul(z1z1, p->z, p->z); fmul(z2z2, q->z, q->z);
    fmul(u1, p->x, z2z2); fmul(u2, q->x, z1z1);
    fmul(s1, p->y, q->z); fmul(s1, s1, z2z2); fmul(s2, q->y, p->z); fmul(s2, s2, z1z1);
    fsub(h, u2, u1); fsub(rr, s2, s1);
    if (bn_is_zero(h, L_)) { if (bn_is_zero(rr, L_)) { pdbl(r, p); return; } bn_zero(r->z, L_); return; }
    fadd(i, h, h); fmul(i, i, i); fmul(j, h, i); fadd(rr, rr, rr); fmul(v, u1, i);
    fmul(x3, rr, rr); fsub(x3, x3, j); fsub(x3, x3, v); fsub(x3, x3, v);
    fsub(t, v, x3); fmul(y3, rr, t); fmul(t, s1, j); fadd(t, t, t); fsub(y3, y3, t);
    fadd(z3, p->z, q->z); fmul(z3, z3, z3); fsub(z3, z3, z1z1); fsub(z3, z3, z2z2); fmul(z3, z3, h);
    bn_copy(r->x, x3, L_); bn_copy(r->y, y3, L_); bn_copy(r->z, z3, L_);
}

int ecdsa_verify(int curve, const uint8_t* pub, size_t publen, const uint8_t* digest, size_t dlen,
                 const uint8_t* r, size_t rlen, const uint8_t* s, size_t slen) {
    const curve_def_t* c = curve == 256 ? &P256 : curve == 384 ? &P384 : 0;
    if (!c) return -1;
    int L = L_ = c->L; size_t bytes = (size_t)L * 4;
    if (publen != 1 + 2 * bytes || pub[0] != 4) return -1;
    bn_t p, n, R, S, e, qx, qy, t, w, u1, u2;
    hexnum(p, L, c->p); hexnum(n, L, c->n);
    if (mont_init(&fp, p, L) || mont_init(&fn, n, L)) return -1;
    if (bn_from(R, L, r, rlen) || bn_from(S, L, s, slen)) return -1;
    if (bn_is_zero(R, L) || bn_is_zero(S, L) || bn_cmp(R, n, L) >= 0 || bn_cmp(S, n, L) >= 0) return -1;
    if (bn_from(qx, L, pub + 1, bytes) || bn_from(qy, L, pub + 1 + bytes, bytes)) return -1;
    if (bn_cmp(qx, p, L) >= 0 || bn_cmp(qy, p, L) >= 0) return -1;
    /* точка на кривой: y^2 = x^3 - 3x + b */
    jpt_t Q, G, X;
    to_m(&fp, Q.x, qx); to_m(&fp, Q.y, qy); bn_copy(Q.z, fp.one, L);
    hexnum(t, L, c->b); to_m(&fp, c_b, t);
    bn_zero(t, L); t[0] = 3; to_m(&fp, c_three, t);
    bn_t lhs, rhs;
    fmul(lhs, Q.y, Q.y);
    fmul(rhs, Q.x, Q.x); fmul(rhs, rhs, Q.x); fmul(t, c_three, Q.x); fsub(rhs, rhs, t); fadd(rhs, rhs, c_b);
    if (bn_cmp(lhs, rhs, L) != 0) return -1;
    hexnum(t, L, c->gx); to_m(&fp, G.x, t); hexnum(t, L, c->gy); to_m(&fp, G.y, t); bn_copy(G.z, fp.one, L);
    /* e: левые биты хэша по размеру порядка */
    uint8_t eb[48]; size_t el = dlen < bytes ? dlen : bytes;
    for (size_t i = 0; i < el; i++) eb[i] = digest[i];
    (void)bn_from(e, L, eb, el);
    if (bn_cmp(e, n, L) >= 0) bn_sub(e, e, n, L);
    /* w = s^-1 (Ферма: s^(n-2)), u1 = e*w, u2 = r*w по модулю n */
    bn_t nm2, sm, em_, rm_; bn_zero(t, L); t[0] = 2; bn_sub(nm2, n, t, L);
    to_m(&fn, sm, S); mont_pow(&fn, w, sm, nm2, L);
    to_m(&fn, em_, e); mont_mul(&fn, u1, em_, w); from_m(&fn, u1, u1);
    to_m(&fn, rm_, R); mont_mul(&fn, u2, rm_, w); from_m(&fn, u2, u2);
    /* X = u1*G + u2*Q (Шамир: общий проход по битам) */
    jpt_t GQ; padd(&GQ, &G, &Q);
    bn_zero(X.x, L); bn_zero(X.y, L); bn_zero(X.z, L);
    int top = bn_bits(u1, L) > bn_bits(u2, L) ? bn_bits(u1, L) : bn_bits(u2, L);
    for (int i = top - 1; i >= 0; i--) {
        pdbl(&X, &X);
        int b1 = (u1[i / 32] >> (i % 32)) & 1, b2 = (u2[i / 32] >> (i % 32)) & 1;
        if (b1 && b2) padd(&X, &X, &GQ); else if (b1) padd(&X, &X, &G); else if (b2) padd(&X, &X, &Q);
    }
    if (bn_is_zero(X.z, L)) return -1;
    bn_t zi, z2, pm2, x;                                         /* x = X / Z^2 */
    bn_zero(t, L); t[0] = 2; bn_sub(pm2, p, t, L);
    mont_pow(&fp, zi, X.z, pm2, L); fmul(z2, zi, zi); fmul(x, X.x, z2); from_m(&fp, x, x);
    if (bn_cmp(x, n, L) >= 0) bn_sub(x, x, n, L);
    return bn_cmp(x, R, L) == 0 ? 0 : -1;
}

/* ---------------- Ed25519 (RFC 8032 §5.1.7): проверка подписи ----------------
   Кривая -x^2 + y^2 = 1 + d x^2 y^2 над GF(2^255 - 19), расширенные координаты (X:Y:Z:T), полное сложение
   (add-2008-hwcd-3: годится и для удвоения). Поле - та же арифметика Монтгомери, что у ECDSA, но своя (ep). Проверка
   [S]B = R + [k]A делается как сравнение кодировки [S]B + [k](-A) с R из подписи: числа открыты, постоянное время не нужно. */
typedef uint32_t fe_t[8];
typedef struct { fe_t x, y, z, t; } ept_t;
static mont_t ep;
static fe_t ed_d, ed_d2, ed_i;                                   /* d, 2d, sqrt(-1) - в форме Монтгомери */

static const char ED_P[]   = "7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffed";
static const char ED_PM2[] = "7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeb";   /* p - 2: обратное по Ферма */
static const char ED_E58[] = "0ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffd";   /* (p - 5) / 8 */
static const char ED_E14[] = "1ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffb";   /* (p - 1) / 4: sqrt(-1) = 2^это */
static const char ED_L[]   = "1000000000000000000000000000000014def9dea2f79cd65812631a5cf5d3ed";   /* порядок группы */
static const uint8_t ED_BASE[32] = { 0x58,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
                                     0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66 };   /* y = 4/5, x чётный */

static void ea(uint32_t* r, const uint32_t* a, const uint32_t* b) { if (bn_add(r, a, b, 8) || bn_cmp(r, ep.n, 8) >= 0) bn_sub(r, r, ep.n, 8); }
static void es(uint32_t* r, const uint32_t* a, const uint32_t* b) { if (bn_sub(r, a, b, 8)) bn_add(r, r, ep.n, 8); }
static void em(uint32_t* r, const uint32_t* a, const uint32_t* b) { mont_mul(&ep, r, a, b); }
static void epow(uint32_t* r, const uint32_t* a, const char* hexe) { fe_t e; hexnum(e, 8, hexe); mont_pow(&ep, r, a, e, 8); }
static void e_from_int(uint32_t* r, uint32_t v) { fe_t t; bn_zero(t, 8); t[0] = v; to_m(&ep, r, t); }

static int ed_init(void) {
    fe_t p, a, b, t;
    hexnum(p, 8, ED_P);
    if (mont_init(&ep, p, 8)) return -1;
    e_from_int(a, 121665); e_from_int(b, 121666);
    epow(t, b, ED_PM2); em(t, a, t);                              /* 121665 / 121666 */
    bn_zero(a, 8); es(ed_d, a, t);                                /* d = -121665/121666 */
    ea(ed_d2, ed_d, ed_d);
    e_from_int(a, 2); epow(ed_i, a, ED_E14);                      /* sqrt(-1) = 2^((p-1)/4) */
    return 0;
}

static void ept_id(ept_t* r) { bn_zero(r->x, 8); bn_copy(r->y, ep.one, 8); bn_copy(r->z, ep.one, 8); bn_zero(r->t, 8); }
static void ept_add(ept_t* r, const ept_t* p, const ept_t* q) {
    fe_t a, b, c, d, e, f, g, h, t1, t2;
    es(t1, p->y, p->x); es(t2, q->y, q->x); em(a, t1, t2);
    ea(t1, p->y, p->x); ea(t2, q->y, q->x); em(b, t1, t2);
    em(c, p->t, q->t); em(c, c, ed_d2);
    em(d, p->z, q->z); ea(d, d, d);
    es(e, b, a); es(f, d, c); ea(g, d, c); ea(h, b, a);
    em(r->x, e, f); em(r->y, g, h); em(r->t, e, h); em(r->z, f, g);
}
/* r = k * p, k - число из 8 частей (двоичный метод слева направо). */
static void ept_mul(ept_t* r, const ept_t* p, const uint32_t* k) {
    ept_t acc; ept_id(&acc);
    for (int i = bn_bits(k, 8) - 1; i >= 0; i--) {
        ept_add(&acc, &acc, &acc);
        if ((k[i / 32] >> (i % 32)) & 1) ept_add(&acc, &acc, p);
    }
    *r = acc;
}
/* Число из 32 байт в порядке «младший первый»; 0 - ок. */
static void le_to_bn(uint32_t* a, int L, const uint8_t* b, size_t n) {
    bn_zero(a, L);
    for (size_t i = 0; i < n; i++) a[i / 4] |= (uint32_t)b[i] << (8 * (i % 4));
}
/* Распаковка точки (RFC 8032 §5.1.3): 0 - ок. */
static int ept_decode(ept_t* r, const uint8_t enc[32]) {
    fe_t y, yn, p, y2, u, v, v3, v7, x, t, c, one;
    uint8_t buf[32]; for (int i = 0; i < 32; i++) buf[i] = enc[i];
    int sign = buf[31] >> 7; buf[31] &= 0x7f;
    le_to_bn(yn, 8, buf, 32); hexnum(p, 8, ED_P);
    if (bn_cmp(yn, p, 8) >= 0) return -1;                         /* y >= p: не каноническая запись */
    to_m(&ep, y, yn);
    bn_copy(one, ep.one, 8);
    em(y2, y, y); es(u, y2, one); em(v, y2, ed_d); ea(v, v, one);   /* u = y^2 - 1, v = d y^2 + 1 */
    em(v3, v, v); em(v3, v3, v);                                  /* v^3 */
    em(v7, v3, v3); em(v7, v7, v);                                /* v^7 */
    em(t, u, v7); epow(t, t, ED_E58); em(x, u, v3); em(x, x, t);  /* x = u v^3 (u v^7)^((p-5)/8) */
    em(c, x, x); em(c, c, v);                                     /* v x^2 */
    if (bn_cmp(c, u, 8) != 0) {
        fe_t nu; bn_zero(t, 8); es(nu, t, u);
        if (bn_cmp(c, nu, 8) != 0) return -1;                     /* корня нет: не точка кривой */
        em(x, x, ed_i);
    }
    fe_t xn; from_m(&ep, xn, x);
    if (bn_is_zero(xn, 8) && sign) return -1;
    if ((int)(xn[0] & 1) != sign) { bn_zero(t, 8); es(x, t, x); }
    bn_copy(r->x, x, 8); bn_copy(r->y, y, 8); bn_copy(r->z, one, 8); em(r->t, x, y);
    return 0;
}
static void ept_encode(uint8_t out[32], const ept_t* p) {
    fe_t zi, x, y;
    epow(zi, p->z, ED_PM2); em(x, p->x, zi); em(y, p->y, zi);
    from_m(&ep, x, x); from_m(&ep, y, y);
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(y[i / 4] >> (8 * (i % 4)));
    out[31] |= (uint8_t)((x[0] & 1) << 7);
}

int ed25519_verify(const uint8_t pub[32], const uint8_t* msg, size_t mlen, const uint8_t sig[64]) {
    /* All three buffers are mandatory, including for an empty message. */
    if (!pub || !msg || !sig) return -1;
    fe_t S, L, k, zero;
    ept_t A, B, sB, kA, R;
    if (ed_init()) return -1;
    le_to_bn(S, 8, sig + 32, 32); hexnum(L, 8, ED_L);
    if (bn_cmp(S, L, 8) >= 0) return -1;                          /* S < L обязательно (RFC 8032 §5.1.7) */
    if (ept_decode(&A, pub)) return -1;
    /* Reject identity and all non-prime-subgroup public keys. Without this,
       the identity key admits trivial S=0/R=identity forgeries. */
    ept_t order_check;
    fe_t order_diff;
    ept_mul(&order_check, &A, L);
    es(order_diff, order_check.y, order_check.z);
    if (!bn_is_zero(order_check.x, 8) || !bn_is_zero(order_diff, 8))
        return -1;                                                   /* [L]A must be identity */
    es(order_diff, A.y, A.z);
    if (bn_is_zero(A.x, 8) && bn_is_zero(order_diff, 8))
        return -1;                                                   /* reject identity public key */
    if (ept_decode(&B, ED_BASE)) return -1;
    sha512_ctx_t c; uint8_t h[64];
    sha512_init(&c); sha512_update(&c, sig, 32); sha512_update(&c, pub, 32); sha512_update(&c, msg, mlen); sha512_final(&c, h);
    bn_zero(k, 8);                                                /* k = h mod L: сдвиг по биту с вычитанием */
    for (int i = 511; i >= 0; i--) {
        uint32_t carry = bn_add(k, k, k, 8);
        k[0] |= (uint32_t)((h[i / 8] >> (i % 8)) & 1);
        if (carry || bn_cmp(k, L, 8) >= 0) bn_sub(k, k, L, 8);
    }
    ept_mul(&sB, &B, S);
    bn_zero(zero, 8); es(A.x, zero, A.x); es(A.t, zero, A.t);     /* -A = (-x, y, z, -t) */
    ept_mul(&kA, &A, k);
    ept_add(&R, &sB, &kA);
    uint8_t enc[32]; ept_encode(enc, &R);
    return crypto_eq(enc, sig, 32) ? 0 : -1;
}
