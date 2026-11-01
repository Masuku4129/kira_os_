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

/* ---------------- SHA-512 / SHA-384 ---------------- */
static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL,0x142929670a0e6e70ULL,0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,0xca273eceea26619cULL,0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL };
static uint64_t ror64(uint64_t x, int n) { return x >> n | x << (64 - n); }
static void sha512_block(sha512_ctx_t* c, const uint8_t* p) {
    uint64_t w[80], a[8];
    for (int i = 0; i < 16; i++) { w[i] = 0; for (int k = 0; k < 8; k++) w[i] = w[i] << 8 | p[8 * i + k]; }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (int i = 0; i < 8; i++) a[i] = c->h[i];
    for (int i = 0; i < 80; i++) {
        uint64_t S1 = ror64(a[4], 14) ^ ror64(a[4], 18) ^ ror64(a[4], 41), ch = (a[4] & a[5]) ^ (~a[4] & a[6]);
        uint64_t t1 = a[7] + S1 + ch + K512[i] + w[i];
        uint64_t S0 = ror64(a[0], 28) ^ ror64(a[0], 34) ^ ror64(a[0], 39), mj = (a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]);
        uint64_t t2 = S0 + mj;
        a[7] = a[6]; a[6] = a[5]; a[5] = a[4]; a[4] = a[3] + t1; a[3] = a[2]; a[2] = a[1]; a[1] = a[0]; a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) c->h[i] += a[i];
}
void sha512_init(sha512_ctx_t* c) {
    static const uint64_t iv[8] = { 0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
                                    0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL };
    for (int i = 0; i < 8; i++) c->h[i] = iv[i];
    c->len = 0; c->n = 0; c->out = 64;
}
void sha384_init(sha512_ctx_t* c) {
    static const uint64_t iv[8] = { 0xcbbb9d5dc1059ed8ULL,0x629a292a367cd507ULL,0x9159015a3070dd17ULL,0x152fecd8f70e5939ULL,
                                    0x67332667ffc00b31ULL,0x8eb44a8768581511ULL,0xdb0c2e0d64f98fa7ULL,0x47b5481dbefa4fa4ULL };
    for (int i = 0; i < 8; i++) c->h[i] = iv[i];
    c->len = 0; c->n = 0; c->out = 48;
}
void sha512_update(sha512_ctx_t* c, const void* d, size_t n) {
    const uint8_t* p = (const uint8_t*)d;
    c->len += n;
    while (n) {
        c->buf[c->n++] = *p++; n--;
        if (c->n == 128) { sha512_block(c, c->buf); c->n = 0; }
    }
}
void sha512_final(sha512_ctx_t* c, uint8_t* out) {
    uint64_t bits = c->len * 8;
    c->buf[c->n++] = 0x80;
    if (c->n > 112) { while (c->n < 128) c->buf[c->n++] = 0; sha512_block(c, c->buf); c->n = 0; }
    while (c->n < 120) c->buf[c->n++] = 0;
    for (int i = 0; i < 8; i++) c->buf[120 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha512_block(c, c->buf);
    for (int i = 0; i < c->out; i++) out[i] = (uint8_t)(c->h[i / 8] >> (56 - 8 * (i % 8)));
}

/* ---------------- AES-128 и GCM ---------------- */
static const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,
    0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,
    0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,
    0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,
    0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,
    0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static uint32_t subw(uint32_t w) { return (uint32_t)SBOX[w >> 24] << 24 | (uint32_t)SBOX[(w >> 16) & 255] << 16 | (uint32_t)SBOX[(w >> 8) & 255] << 8 | SBOX[w & 255]; }
static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x >> 7) * 0x1b)); }

static void aes128_expand(uint32_t rk[44], const uint8_t key[16]) {
    static const uint8_t rcon[10] = { 1, 2, 4, 8, 16, 32, 64, 128, 0x1b, 0x36 };
    for (int i = 0; i < 4; i++) rk[i] = (uint32_t)key[4 * i] << 24 | (uint32_t)key[4 * i + 1] << 16 | (uint32_t)key[4 * i + 2] << 8 | key[4 * i + 3];
    for (int i = 4; i < 44; i++) {
        uint32_t t = rk[i - 1];
        if (i % 4 == 0) t = subw(t << 8 | t >> 24) ^ ((uint32_t)rcon[i / 4 - 1] << 24);
        rk[i] = rk[i - 4] ^ t;
    }
}

void aes128_encrypt_block(const uint32_t rk[44], const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ (uint8_t)(rk[i / 4] >> (24 - 8 * (i % 4)));
    for (int r = 1; r <= 10; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = SBOX[s[(i + 4 * (i % 4)) % 16]];          /* SubBytes + ShiftRows */
        if (r < 10)
            for (int c = 0; c < 4; c++) {                                              /* MixColumns */
                uint8_t a0 = t[4 * c], a1 = t[4 * c + 1], a2 = t[4 * c + 2], a3 = t[4 * c + 3], x = a0 ^ a1 ^ a2 ^ a3;
                t[4 * c] ^= x ^ xt(a0 ^ a1); t[4 * c + 1] ^= x ^ xt(a1 ^ a2); t[4 * c + 2] ^= x ^ xt(a2 ^ a3); t[4 * c + 3] ^= x ^ xt(a3 ^ a0);
            }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ (uint8_t)(rk[4 * r + i / 4] >> (24 - 8 * (i % 4)));
    }
    for (int i = 0; i < 16; i++) out[i] = s[i];
    crypto_wipe(s, sizeof s);
}

static void gf_mul(uint8_t x[16], const uint8_t h[16]) {             /* x = x * h в GF(2^128), порядок битов GCM */
    uint8_t z[16] = { 0 }, v[16];
    for (int i = 0; i < 16; i++) v[i] = h[i];
    for (int i = 0; i < 128; i++) {
        uint8_t m = (uint8_t)(0 - ((x[i / 8] >> (7 - i % 8)) & 1));
        for (int k = 0; k < 16; k++) z[k] ^= v[k] & m;
        uint8_t lsb = (uint8_t)(0 - (v[15] & 1));
        for (int k = 15; k > 0; k--) v[k] = (uint8_t)(v[k] >> 1 | v[k - 1] << 7);
        v[0] = (uint8_t)((v[0] >> 1) ^ (0xe1 & lsb));
    }
    for (int i = 0; i < 16; i++) x[i] = z[i];
}
static void ghash_data(uint8_t y[16], const uint8_t h[16], const uint8_t* d, size_t n) {
    while (n) {
        size_t k = n < 16 ? n : 16;
        for (size_t i = 0; i < k; i++) y[i] ^= d[i];
        gf_mul(y, h);
        d += k; n -= k;
    }
}
void aes128_gcm_init(aes_gcm_t* g, const uint8_t key[16]) {
    static const uint8_t z[16];
    aes128_expand(g->rk, key);
    aes128_encrypt_block(g->rk, z, g->h);
}
static void gcm_ctr(const aes_gcm_t* g, const uint8_t nonce[12], const uint8_t* in, uint8_t* out, size_t len) {
    uint8_t cb[16], ks[16];
    for (int i = 0; i < 12; i++) cb[i] = nonce[i];
    uint32_t c = 2;                                                     /* счётчик 1 - для метки */
    while (len) {
        cb[12] = (uint8_t)(c >> 24); cb[13] = (uint8_t)(c >> 16); cb[14] = (uint8_t)(c >> 8); cb[15] = (uint8_t)c; c++;
        aes128_encrypt_block(g->rk, cb, ks);
        size_t k = len < 16 ? len : 16;
        for (size_t i = 0; i < k; i++) out[i] = in[i] ^ ks[i];
        in += k; out += k; len -= k;
    }
}
static void gcm_tag(const aes_gcm_t* g, const uint8_t nonce[12], const uint8_t* aad, size_t alen, const uint8_t* ct, size_t clen, uint8_t tag[16]) {
    uint8_t y[16] = { 0 }, lens[16], j0[16], ek[16];
    ghash_data(y, g->h, aad, alen);
    ghash_data(y, g->h, ct, clen);
    uint64_t ab = (uint64_t)alen * 8, cbits = (uint64_t)clen * 8;
    for (int i = 0; i < 8; i++) { lens[i] = (uint8_t)(ab >> (56 - 8 * i)); lens[8 + i] = (uint8_t)(cbits >> (56 - 8 * i)); }
    ghash_data(y, g->h, lens, 16);
    for (int i = 0; i < 12; i++) j0[i] = nonce[i];
    j0[12] = 0; j0[13] = 0; j0[14] = 0; j0[15] = 1;
    aes128_encrypt_block(g->rk, j0, ek);
    for (int i = 0; i < 16; i++) tag[i] = y[i] ^ ek[i];
}
void aes128_gcm_seal(const aes_gcm_t* g, const uint8_t nonce[12], const uint8_t* aad, size_t alen,
                     const uint8_t* in, size_t len, uint8_t* out, uint8_t tag[16]) {
    gcm_ctr(g, nonce, in, out, len);
    gcm_tag(g, nonce, aad, alen, out, len, tag);
}
int aes128_gcm_open(const aes_gcm_t* g, const uint8_t nonce[12], const uint8_t* aad, size_t alen,
                    const uint8_t* in, size_t len, const uint8_t tag[16], uint8_t* out) {
    uint8_t t[16];
    gcm_tag(g, nonce, aad, alen, in, len, t);
    if (!crypto_eq(t, tag, 16)) return -1;
    gcm_ctr(g, nonce, in, out, len);
    return 0;
}
