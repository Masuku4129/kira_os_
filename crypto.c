/* Криптография TLS 1.3: ChaCha20, Poly1305, AEAD, X25519, HMAC-SHA256, HKDF. Только целочисленная арифметика,
   без ветвлений по секретам (X25519 - лестница Монтгомери с выбором по маске, Poly1305 - сравнение по маске). */
#include "crypto.h"

static uint32_t ld32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void st32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t rotl(uint32_t v, int n) { return v << n | v >> (32 - n); }

void crypto_wipe(void* p, size_t n) { volatile uint8_t* q = (volatile uint8_t*)p; while (n--) *q++ = 0; }
int crypto_eq(const void* a, const void* b, size_t n) {
    const uint8_t* x = (const uint8_t*)a; const uint8_t* y = (const uint8_t*)b;
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= x[i] ^ y[i];
    return d == 0;
}

/* ---------------- ChaCha20 ---------------- */
#define QR(a, b, c, d) a += b; d = rotl(d ^ a, 16); c += d; b = rotl(b ^ c, 12); a += b; d = rotl(d ^ a, 8); c += d; b = rotl(b ^ c, 7)

void chacha20_block(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], uint8_t out[64]) {
    uint32_t s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = ld32(key + 4 * i);
    s[12] = counter;
    for (int i = 0; i < 3; i++) s[13 + i] = ld32(nonce + 4 * i);
    for (int i = 0; i < 16; i++) x[i] = s[i];
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]); QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]); QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (int i = 0; i < 16; i++) st32(out + 4 * i, x[i] + s[i]);
    crypto_wipe(x, sizeof x); crypto_wipe(s, sizeof s);
}

void chacha20_xor(const uint8_t key[32], uint32_t counter, const uint8_t nonce[12], const uint8_t* in, uint8_t* out, size_t len) {
    uint8_t ks[64];
    while (len) {
        chacha20_block(key, counter++, nonce, ks);
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; i++) out[i] = in[i] ^ ks[i];
        in += n; out += n; len -= n;
    }
    crypto_wipe(ks, sizeof ks);
}

/* ---------------- Poly1305 (пять 26-битных частей) ---------------- */
void poly1305_init(poly1305_ctx_t* c, const uint8_t key[32]) {
    c->r[0] = ld32(key + 0) & 0x3ffffff;
    c->r[1] = (ld32(key + 3) >> 2) & 0x3ffff03;
    c->r[2] = (ld32(key + 6) >> 4) & 0x3ffc0ff;
    c->r[3] = (ld32(key + 9) >> 6) & 0x3f03fff;
    c->r[4] = (ld32(key + 12) >> 8) & 0x00fffff;
    for (int i = 0; i < 5; i++) c->h[i] = 0;
    for (int i = 0; i < 4; i++) c->pad[i] = ld32(key + 16 + 4 * i);
    c->left = 0;
}

static void poly_blocks(poly1305_ctx_t* c, const uint8_t* m, size_t n, uint32_t hibit) {
    const uint32_t r0 = c->r[0], r1 = c->r[1], r2 = c->r[2], r3 = c->r[3], r4 = c->r[4];
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = c->h[0], h1 = c->h[1], h2 = c->h[2], h3 = c->h[3], h4 = c->h[4];
    while (n >= 16) {
        h0 += ld32(m + 0) & 0x3ffffff;
        h1 += (ld32(m + 3) >> 2) & 0x3ffffff;
        h2 += (ld32(m + 6) >> 4) & 0x3ffffff;
        h3 += (ld32(m + 9) >> 6) & 0x3ffffff;
        h4 += (ld32(m + 12) >> 8) | hibit;
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t cy;
        cy = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff; d1 += cy;
        cy = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff; d2 += cy;
        cy = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff; d3 += cy;
        cy = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff; d4 += cy;
        cy = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += cy * 5; cy = h0 >> 26; h0 &= 0x3ffffff; h1 += cy;
        m += 16; n -= 16;
    }
    c->h[0] = h0; c->h[1] = h1; c->h[2] = h2; c->h[3] = h3; c->h[4] = h4;
}

void poly1305_update(poly1305_ctx_t* c, const uint8_t* m, size_t n) {
    if (c->left) {
        while (n && c->left < 16) { c->buf[c->left++] = *m++; n--; }
        if (c->left < 16) return;
        poly_blocks(c, c->buf, 16, 1u << 24);
        c->left = 0;
    }
    size_t full = n & ~(size_t)15;
    if (full) { poly_blocks(c, m, full, 1u << 24); m += full; n -= full; }
    while (n) { c->buf[c->left++] = *m++; n--; }
}

void poly1305_final(poly1305_ctx_t* c, uint8_t tag[16]) {
    if (c->left) {
        c->buf[c->left] = 1;
        for (size_t i = c->left + 1; i < 16; i++) c->buf[i] = 0;
        poly_blocks(c, c->buf, 16, 0);
    }
    uint32_t h0 = c->h[0], h1 = c->h[1], h2 = c->h[2], h3 = c->h[3], h4 = c->h[4], cy;
    cy = h1 >> 26; h1 &= 0x3ffffff; h2 += cy;
    cy = h2 >> 26; h2 &= 0x3ffffff; h3 += cy;
    cy = h3 >> 26; h3 &= 0x3ffffff; h4 += cy;
    cy = h4 >> 26; h4 &= 0x3ffffff; h0 += cy * 5;
    cy = h0 >> 26; h0 &= 0x3ffffff; h1 += cy;
    uint32_t g0 = h0 + 5; cy = g0 >> 26; g0 &= 0x3ffffff;           /* h - p: если не отрицательно, берётся оно */
    uint32_t g1 = h1 + cy; cy = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + cy; cy = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + cy; cy = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + cy - (1u << 26);
    uint32_t mask = (g4 >> 31) - 1;                                  /* все единицы - взять g */
    h0 = (h0 & ~mask) | (g0 & mask); h1 = (h1 & ~mask) | (g1 & mask); h2 = (h2 & ~mask) | (g2 & mask);
    h3 = (h3 & ~mask) | (g3 & mask); h4 = (h4 & ~mask) | (g4 & mask);
    h0 = h0 | h1 << 26; h1 = h1 >> 6 | h2 << 20; h2 = h2 >> 12 | h3 << 14; h3 = h3 >> 18 | h4 << 8;
    uint64_t f;
    f = (uint64_t)h0 + c->pad[0];             h0 = (uint32_t)f;
    f = (uint64_t)h1 + c->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + c->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + c->pad[3] + (f >> 32); h3 = (uint32_t)f;
    st32(tag, h0); st32(tag + 4, h1); st32(tag + 8, h2); st32(tag + 12, h3);
    crypto_wipe(c, sizeof *c);
}

/* ---------------- AEAD ChaCha20-Poly1305 ---------------- */
static void aead_tag(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t alen,
                     const uint8_t* ct, size_t clen, uint8_t tag[16]) {
    uint8_t blk[64], pad[16] = { 0 }, lens[16];
    chacha20_block(key, 0, nonce, blk);                              /* одноразовый ключ Poly1305 - блок 0 */
    poly1305_ctx_t p;
    poly1305_init(&p, blk);
    poly1305_update(&p, aad, alen);
    if (alen % 16) poly1305_update(&p, pad, 16 - alen % 16);
    poly1305_update(&p, ct, clen);
    if (clen % 16) poly1305_update(&p, pad, 16 - clen % 16);
    for (int i = 0; i < 8; i++) { lens[i] = (uint8_t)((uint64_t)alen >> (8 * i)); lens[8 + i] = (uint8_t)((uint64_t)clen >> (8 * i)); }
    poly1305_update(&p, lens, 16);
    poly1305_final(&p, tag);
    crypto_wipe(blk, sizeof blk);
}

void aead_chacha20_poly1305_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aad_len,
                                 const uint8_t* in, size_t len, uint8_t* out, uint8_t tag[16]) {
    chacha20_xor(key, 1, nonce, in, out, len);
    aead_tag(key, nonce, aad, aad_len, out, len, tag);
}

int aead_chacha20_poly1305_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aad_len,
                                const uint8_t* in, size_t len, const uint8_t tag[16], uint8_t* out) {
    uint8_t t[16];
    aead_tag(key, nonce, aad, aad_len, in, len, t);
    int ok = crypto_eq(t, tag, 16);
    crypto_wipe(t, sizeof t);
    if (!ok) return -1;
    chacha20_xor(key, 1, nonce, in, out, len);
    return 0;
}

/* ---------------- X25519: поле 2^255-19, 16 частей по 16 бит ---------------- */
typedef int64_t gf[16];

static void car(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += 1LL << 16;
        int64_t c = o[i] >> 16;
        if (i < 15) o[i + 1] += c - 1; else o[0] += 38 * (c - 1);
        o[i] -= c * 65536;
    }
}
static void sel(gf p, gf q, int b) {
    int64_t m = -(int64_t)b;
    for (int i = 0; i < 16; i++) { int64_t t = m & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; }
}
static void pack(uint8_t o[32], const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car(t); car(t); car(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2 * i] = (uint8_t)(t[i] & 0xff); o[2 * i + 1] = (uint8_t)(t[i] >> 8); }
}
static void unpack(gf o, const uint8_t n[32]) {
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}
static void fadd(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void fsub(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void fmul(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car(o); car(o);
}
static void fsq(gf o, const gf a) { fmul(o, a, a); }
static void finv(gf o, const gf i) {
    gf c;
    for (int k = 0; k < 16; k++) c[k] = i[k];
    for (int a = 253; a >= 0; a--) { fsq(c, c); if (a != 2 && a != 4) fmul(c, c, i); }
    for (int k = 0; k < 16; k++) o[k] = c[k];
}

int x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    static const gf k121665 = { 0xDB41, 1 };
    uint8_t z[32];
    gf x, a, b, c, d, e, f;
    for (int i = 0; i < 32; i++) z[i] = scalar[i];
    z[31] = (uint8_t)((z[31] & 127) | 64); z[0] &= 248;
    unpack(x, point);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; a[i] = c[i] = d[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; i--) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel(a, b, r); sel(c, d, r);
        fadd(e, a, c); fsub(a, a, c); fadd(c, b, d); fsub(b, b, d);
        fsq(d, e); fsq(f, a); fmul(a, c, a); fmul(c, b, e);
        fadd(e, a, c); fsub(a, a, c); fsq(b, a); fsub(c, d, f);
        fmul(a, c, k121665); fadd(a, a, d); fmul(c, c, a); fmul(a, d, f);
        fmul(d, b, x); fsq(b, e);
        sel(a, b, r); sel(c, d, r);
    }
    finv(c, c); fmul(a, a, c);
    pack(out, a);
    crypto_wipe(z, sizeof z); crypto_wipe(a, sizeof a); crypto_wipe(b, sizeof b);
    crypto_wipe(c, sizeof c); crypto_wipe(d, sizeof d); crypto_wipe(e, sizeof e); crypto_wipe(f, sizeof f);
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= out[i];
    return acc ? 0 : -1;                                             /* точка малого порядка: общий секрет нулевой */
}

void x25519_base(uint8_t out[32], const uint8_t scalar[32]) {
    static const uint8_t nine[32] = { 9 };
    (void)x25519(out, scalar, nine);
}

/* ---------------- HMAC-SHA256, HKDF ---------------- */
void hmac_sha256(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen, uint8_t out[32]) {
    uint8_t k[64], pad[64], ih[32];
    for (int i = 0; i < 64; i++) k[i] = 0;
    if (klen > 64) sha256(key, klen, k); else for (size_t i = 0; i < klen; i++) k[i] = key[i];
    sha256_ctx_t c;
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&c); sha256_update(&c, pad, 64); sha256_update(&c, msg, mlen); sha256_final(&c, ih);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&c); sha256_update(&c, pad, 64); sha256_update(&c, ih, 32); sha256_final(&c, out);
    crypto_wipe(k, sizeof k); crypto_wipe(pad, sizeof pad); crypto_wipe(ih, sizeof ih); crypto_wipe(&c, sizeof c);
}

void hkdf_extract(const uint8_t* salt, size_t slen, const uint8_t* ikm, size_t ilen, uint8_t prk[32]) {
    static const uint8_t zero[32];
    if (!salt || !slen) { salt = zero; slen = 32; }
    hmac_sha256(salt, slen, ikm, ilen, prk);
}

void hkdf_expand(const uint8_t prk[32], const uint8_t* info, size_t ilen, uint8_t* out, size_t olen) {
    uint8_t t[32], msg[32 + 300 + 1];
    size_t tl = 0, done = 0;
    if (ilen > 300) ilen = 300;
    for (uint8_t i = 1; done < olen; i++) {
        size_t m = 0;
        for (size_t k = 0; k < tl; k++) msg[m++] = t[k];
        for (size_t k = 0; k < ilen; k++) msg[m++] = info[k];
        msg[m++] = i;
        hmac_sha256(prk, 32, msg, m, t);
        tl = 32;
        for (size_t k = 0; k < 32 && done < olen; k++) out[done++] = t[k];
    }
    crypto_wipe(t, sizeof t); crypto_wipe(msg, sizeof msg);
}

void hkdf_expand_label(const uint8_t secret[32], const char* label, const uint8_t* ctx, size_t clen, uint8_t* out, size_t olen) {
    uint8_t info[2 + 1 + 6 + 64 + 1 + 64];
    size_t n = 0, ll = 0;
    while (label[ll] && ll < 64) ll++;
    if (clen > 64) clen = 64;
    info[n++] = (uint8_t)(olen >> 8); info[n++] = (uint8_t)olen;
    info[n++] = (uint8_t)(6 + ll);
    const char* p = "tls13 ";
    for (int i = 0; i < 6; i++) info[n++] = (uint8_t)p[i];
    for (size_t i = 0; i < ll; i++) info[n++] = (uint8_t)label[i];
    info[n++] = (uint8_t)clen;
    for (size_t i = 0; i < clen; i++) info[n++] = ctx[i];
    hkdf_expand(secret, info, n, out, olen);
}
