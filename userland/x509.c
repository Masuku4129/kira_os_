/* X.509: разбор DER и проверка цепочки. Всё, что не распознано или не поддерживается, - отказ, а не «пропустить». */
#include "x509.h"
#include "../pk.h"
#include "../crypto.h"

typedef struct { const uint8_t* p; size_t n; } der_t;

/* Следующий элемент TLV: тег, значение, весь элемент. 0 - есть. */
static int der_next(der_t* in, uint8_t* tag, der_t* val, der_t* whole) {
    if (in->n < 2) return -1;
    const uint8_t* s = in->p;
    uint8_t t = s[0]; size_t i = 1, l = s[i++];
    if ((t & 0x1F) == 0x1F) return -1;                           /* многобайтовые теги в сертификатах не встречаются */
    if (l & 0x80) {
        unsigned k = l & 0x7F;
        if (k == 0 || k > 4 || i + k > in->n) return -1;
        l = 0;
        for (unsigned j = 0; j < k; j++) l = l << 8 | s[i++];
    }
    if (l > in->n - i) return -1;
    if (tag) *tag = t;
    if (val) { val->p = s + i; val->n = l; }
    if (whole) { whole->p = s; whole->n = i + l; }
    in->p += i + l; in->n -= i + l;
    return 0;
}
static int der_expect(der_t* in, uint8_t tag, der_t* val, der_t* whole) {
    uint8_t t;
    der_t save = *in;
    if (der_next(in, &t, val, whole) || t != tag) { *in = save; return -1; }
    return 0;
}
static int oid_is(const der_t* o, const char* hex) {
    size_t n = 0;
    for (const char* h = hex; h[0] && h[1]; h += 2, n++) {
        int v = 0;
        for (int k = 0; k < 2; k++) { char c = h[k]; v = v * 16 + (c <= '9' ? c - '0' : c - 'a' + 10); }
        if (n >= o->n || o->p[n] != (uint8_t)v) return 0;
    }
    return n == o->n;
}

static int same(const uint8_t* a, size_t an, const uint8_t* b, size_t bn) {
    if (an != bn) return 0;
    for (size_t i = 0; i < an; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static int64_t days_from_civil(int64_t y, int m, int d) {          /* Х. Хиннант: дни от 1970-01-01 */
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static int dig(const uint8_t* p, int n, int* v) {
    *v = 0;
    for (int i = 0; i < n; i++) { if (p[i] < '0' || p[i] > '9') return -1; *v = *v * 10 + (p[i] - '0'); }
    return 0;
}
static int der_time(der_t* in, int64_t* out) {
    uint8_t t; der_t v;
    if (der_next(in, &t, &v, 0)) return -1;
    int y, mo, d, h, mi, s; const uint8_t* p = v.p;
    if (t == 0x17 && v.n == 13) { if (dig(p, 2, &y)) return -1; y += y < 50 ? 2000 : 1900; p += 2; }
    else if (t == 0x18 && v.n == 15) { if (dig(p, 4, &y)) return -1; p += 4; }
    else return -1;
    if (dig(p, 2, &mo) || dig(p + 2, 2, &d) || dig(p + 4, 2, &h) || dig(p + 6, 2, &mi) || dig(p + 8, 2, &s) || p[10] != 'Z') return -1;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) return -1;
    *out = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    return 0;
}

static int hash_of_oid(const der_t* o) {
    if (oid_is(o, "608648016503040201")) return PK_SHA256;
    if (oid_is(o, "608648016503040202")) return PK_SHA384;
    if (oid_is(o, "608648016503040203")) return PK_SHA512;
    return 0;                                                     /* SHA-1 и прочее - не поддерживается */
}
/* RSASSA-PSS-params (RFC 4055): [0] хэш, [1] MGF1 с хэшем, [2] соль, [3] trailerField (только 1). Умолчания - SHA-1, то есть
   неподдерживаемое: хэш и MGF обязаны быть названы. 0 - разобрано. */
static int pss_params(der_t p, int* h, int* m, int* s) {
    *h = *m = 0; *s = 20;
    while (p.n) {
        uint8_t t; der_t v, a, oid;
        if (der_next(&p, &t, &v, 0)) return -1;
        if (t == 0xA0) {
            if (der_expect(&v, 0x30, &a, 0) || der_expect(&a, 0x06, &oid, 0) || !(*h = hash_of_oid(&oid))) return -1;
        } else if (t == 0xA1) {
            der_t g, ga;
            if (der_expect(&v, 0x30, &g, 0) || der_expect(&g, 0x06, &oid, 0) || !oid_is(&oid, "2a864886f70d010108")) return -1;
            if (der_expect(&g, 0x30, &ga, 0) || der_expect(&ga, 0x06, &oid, 0) || !(*m = hash_of_oid(&oid))) return -1;
        } else if (t == 0xA2) {
            der_t i;
            if (der_expect(&v, 0x02, &i, 0) || i.n < 1 || i.n > 2 || (i.p[0] & 0x80)) return -1;
            *s = 0; for (size_t k = 0; k < i.n; k++) *s = *s << 8 | i.p[k];
            if (*s > 512) return -1;
        } else if (t == 0xA3) {
            der_t i;
            if (der_expect(&v, 0x02, &i, 0) || i.n != 1 || i.p[0] != 1) return -1;
        } else return -1;
    }
    return *h && *m ? 0 : -1;
}

/* AlgorithmIdentifier (содержимое) -> X509_SIG_*; для RSA-PSS ещё параметры. */
static int sig_alg_of(der_t alg, int* ph, int* pm, int* ps) {
    der_t oid;
    if (der_expect(&alg, 0x06, &oid, 0)) return X509_SIG_NONE;
    if (oid_is(&oid, "2a864886f70d01010b")) return X509_SIG_RSA_SHA256;
    if (oid_is(&oid, "2a864886f70d01010c")) return X509_SIG_RSA_SHA384;
    if (oid_is(&oid, "2a864886f70d01010d")) return X509_SIG_RSA_SHA512;
    if (oid_is(&oid, "2a8648ce3d040302")) return X509_SIG_EC_SHA256;
    if (oid_is(&oid, "2a8648ce3d040303")) return X509_SIG_EC_SHA384;
    if (oid_is(&oid, "2a8648ce3d040304")) return X509_SIG_EC_SHA512;
    if (oid_is(&oid, "2b6570")) return alg.n ? X509_SIG_NONE : X509_SIG_ED25519;      /* параметров у Ed25519 нет (RFC 8410) */
    if (oid_is(&oid, "2a864886f70d01010a")) {                     /* id-RSASSA-PSS: параметры обязательны */
        der_t pr;
        if (der_expect(&alg, 0x30, &pr, 0) || alg.n || pss_params(pr, ph, pm, ps)) return X509_SIG_NONE;
        return X509_SIG_RSA_PSS;
    }
    return X509_SIG_NONE;
}

static int parse_spki(der_t spki, x509_t* c) {
    der_t alg, oid, bits;
    if (der_expect(&spki, 0x30, &alg, 0) || der_expect(&alg, 0x06, &oid, 0) || der_expect(&spki, 0x03, &bits, 0)) return -1;
    if (bits.n < 2 || bits.p[0] != 0) return -1;
    der_t key = { bits.p + 1, bits.n - 1 };
    c->spk = key.p; c->spk_len = key.n;
    int rsa_pss = oid_is(&oid, "2a864886f70d01010a");
    if (oid_is(&oid, "2a864886f70d010101") || rsa_pss) {         /* RSA: RSAPublicKey { n, e }; id-RSASSA-PSS - тот же ключ + ограничения */
        der_t seq, n, e;
        if (der_expect(&key, 0x30, &seq, 0) || der_expect(&seq, 0x02, &n, 0) || der_expect(&seq, 0x02, &e, 0)) return -1;
        c->key_type = rsa_pss ? X509_KEY_RSA_PSS : X509_KEY_RSA; c->rsa_n = n.p; c->rsa_n_len = n.n; c->rsa_e = e.p; c->rsa_e_len = e.n;
        if (rsa_pss && alg.n) {
            der_t pr;
            if (der_expect(&alg, 0x30, &pr, 0) || pss_params(pr, &c->kp_hash, &c->kp_mgf, &c->kp_salt)) { c->key_type = X509_KEY_NONE; c->kp_hash = 0; }
        }
        return 0;
    }
    if (oid_is(&oid, "2b6570")) {                                /* Ed25519 (RFC 8410): 32 байта, параметров нет */
        if (alg.n || key.n != 32) return -1;
        c->key_type = X509_KEY_ED25519; c->ed_pub = key.p; c->ed_pub_len = 32;
        return 0;
    }
    if (oid_is(&oid, "2a8648ce3d0201")) {                        /* EC: параметр - именованная кривая */
        der_t curve;
        if (der_expect(&alg, 0x06, &curve, 0)) return -1;
        if (oid_is(&curve, "2a8648ce3d030107")) c->key_type = X509_KEY_EC256;
        else if (oid_is(&curve, "2b81040022")) c->key_type = X509_KEY_EC384;
        else return 0;                                            /* другая кривая: ключ не поддерживается */
        c->ec_pub = key.p; c->ec_pub_len = key.n;
        return 0;
    }
    return 0;                                                     /* неизвестный тип ключа: KEY_NONE */
}

static int parse_ext(der_t exts, x509_t* c) {
    der_t list;
    if (der_expect(&exts, 0x30, &list, 0)) return -1;
    while (list.n) {
        der_t ext, oid, val; uint8_t t;
        if (der_expect(&list, 0x30, &ext, 0) || der_expect(&ext, 0x06, &oid, 0)) return -1;
        der_t save = ext; der_t crit;
        if (der_next(&ext, &t, &crit, 0)) return -1;
        if (t != 0x01) ext = save;                                /* critical - необязателен */
        if (der_expect(&ext, 0x04, &val, 0)) return -1;
        if (oid_is(&oid, "551d13")) {                            /* basicConstraints { cA BOOLEAN DEFAULT FALSE, ... } */
            der_t seq, b;
            c->has_bc = 1;
            if (der_expect(&val, 0x30, &seq, 0)) return -1;
            if (!der_expect(&seq, 0x01, &b, 0) && b.n == 1 && b.p[0]) c->is_ca = 1;
        } else if (oid_is(&oid, "551d25")) {                     /* extKeyUsage: SEQUENCE OF OID */
            der_t seq, k;
            if (der_expect(&val, 0x30, &seq, 0)) return -1;
            while (seq.n) { if (der_expect(&seq, 0x06, &k, 0)) return -1; if (oid_is(&k, "2b06010505070309")) c->eku_ocsp = 1; }
        } else if (oid_is(&oid, "551d11")) {                     /* subjectAltName */
            der_t seq;
            if (der_expect(&val, 0x30, &seq, 0)) return -1;
            c->san = seq.p; c->san_len = seq.n;
        }
    }
    return 0;
}

int x509_parse(const uint8_t* der, size_t len, x509_t* c) {
    for (size_t i = 0; i < sizeof *c; i++) ((uint8_t*)c)[i] = 0;
    der_t in = { der, len }, cert, tbs, tbsw, alg, algw, sig, v;
    if (der_expect(&in, 0x30, &cert, 0) || der_expect(&cert, 0x30, &tbs, &tbsw) || der_expect(&cert, 0x30, &alg, &algw) ||
        der_expect(&cert, 0x03, &sig, 0)) return -1;
    if (sig.n < 2 || sig.p[0] != 0) return -1;
    c->tbs = tbsw.p; c->tbs_len = tbsw.n; c->sig = sig.p + 1; c->sig_len = sig.n - 1;
    c->sig_alg = sig_alg_of(alg, &c->pss_hash, &c->pss_mgf, &c->pss_salt);
    der_t ver;
    if (!der_expect(&tbs, 0xA0, &ver, 0)) { /* версия: v3 */ }
    der_t ialg, ialgw, iss, issw, val, sub, subw, spki;
    if (der_expect(&tbs, 0x02, &v, 0)) return -2;
    c->serial = v.p; c->serial_len = v.n;
    if (der_expect(&tbs, 0x30, &ialg, &ialgw) || der_expect(&tbs, 0x30, &iss, &issw) ||
        der_expect(&tbs, 0x30, &val, 0) || der_expect(&tbs, 0x30, &sub, &subw) || der_expect(&tbs, 0x30, &spki, 0)) return -2;
    if (!same(ialgw.p, ialgw.n, algw.p, algw.n)) return -2;      /* AlgorithmIdentifier в tbs и снаружи обязан совпадать (RFC 5280 4.1.1.2) */
    c->issuer = issw.p; c->issuer_len = issw.n; c->subject = subw.p; c->subject_len = subw.n;
    if (der_time(&val, &c->not_before) || der_time(&val, &c->not_after)) return -3;
    if (parse_spki(spki, c)) return -4;
    while (tbs.n) {
        uint8_t t; der_t x;
        if (der_next(&tbs, &t, &x, 0)) return -5;
        if (t == 0xA3 && parse_ext(x, c)) return -5;
    }
    return 0;
}

static int alg_hash(int sig_alg) {
    switch (sig_alg) {
    case X509_SIG_RSA_SHA256: case X509_SIG_EC_SHA256: return PK_SHA256;
    case X509_SIG_RSA_SHA384: case X509_SIG_EC_SHA384: return PK_SHA384;
    case X509_SIG_RSA_SHA512: case X509_SIG_EC_SHA512: return PK_SHA512;
    }
    return 0;
}

static int ec_sig(const x509_t* k, const uint8_t* digest, size_t dl, const uint8_t* sig, size_t sl) {
    der_t in = { sig, sl }, seq, r, s;
    if (der_expect(&in, 0x30, &seq, 0) || der_expect(&seq, 0x02, &r, 0) || der_expect(&seq, 0x02, &s, 0) || seq.n || in.n) return -1;
    return ecdsa_verify(k->key_type == X509_KEY_EC256 ? 256 : 384, k->ec_pub, k->ec_pub_len, digest, dl, r.p, r.n, s.p, s.n);
}

/* RSA-PSS ключом k: хэш, MGF1, соль; ключ id-RSASSA-PSS с параметрами допускает только их (соль - не меньше заданной). */
static int pss_sig(const x509_t* k, int h, int mgf, int salt, const uint8_t* data, size_t n, const uint8_t* sig, size_t slen) {
    if (k->key_type != X509_KEY_RSA && k->key_type != X509_KEY_RSA_PSS) return -1;
    if (k->key_type == X509_KEY_RSA_PSS && k->kp_hash && (k->kp_hash != h || k->kp_mgf != mgf || salt < k->kp_salt)) return -1;
    uint8_t d[64];
    pk_hash(h, data, n, d);
    return rsa_verify_pss_ex(k->rsa_n, k->rsa_n_len, k->rsa_e, k->rsa_e_len, h, mgf, (size_t)salt, d, sig, slen);
}

int x509_check_sig(const x509_t* child, const x509_t* parent) {
    if (child->sig_alg == X509_SIG_ED25519) {
        if (parent->key_type != X509_KEY_ED25519 || child->sig_len != 64) return -1;
        return ed25519_verify(parent->ed_pub, child->tbs, child->tbs_len, child->sig);
    }
    if (child->sig_alg == X509_SIG_RSA_PSS)
        return pss_sig(parent, child->pss_hash, child->pss_mgf, child->pss_salt, child->tbs, child->tbs_len, child->sig, child->sig_len);
    int h = alg_hash(child->sig_alg);
    if (!h) return -2;
    uint8_t d[64];
    pk_hash(h, child->tbs, child->tbs_len, d);
    int rsa = child->sig_alg <= X509_SIG_RSA_SHA512;
    if (rsa) {
        if (parent->key_type != X509_KEY_RSA) return -1;
        return rsa_verify_pkcs1(parent->rsa_n, parent->rsa_n_len, parent->rsa_e, parent->rsa_e_len, h, d, child->sig, child->sig_len);
    }
    if (parent->key_type != X509_KEY_EC256 && parent->key_type != X509_KEY_EC384) return -1;
    return ec_sig(parent, d, (size_t)h, child->sig, child->sig_len);
}

int x509_verify_scheme(const x509_t* c, unsigned scheme, const uint8_t* data, size_t n, const uint8_t* sig, size_t slen) {
    uint8_t d[64];
    switch (scheme) {
    case 0x0403: if (c->key_type != X509_KEY_EC256) return -1; pk_hash(PK_SHA256, data, n, d); return ec_sig(c, d, 32, sig, slen);
    case 0x0503: if (c->key_type != X509_KEY_EC384) return -1; pk_hash(PK_SHA384, data, n, d); return ec_sig(c, d, 48, sig, slen);
    case 0x0807:                                                   /* Ed25519: подписываются сами данные, без хэша */
        if (c->key_type != X509_KEY_ED25519 || slen != 64) return -1;
        return ed25519_verify(c->ed_pub, data, n, sig);
    case 0x0804: case 0x0805: case 0x0806: case 0x0809: case 0x080a: case 0x080b: {
        int pss_key = scheme >= 0x0809;                            /* rsae - ключ rsaEncryption; pss - ключ id-RSASSA-PSS */
        if (c->key_type != (pss_key ? X509_KEY_RSA_PSS : X509_KEY_RSA)) return -1;
        unsigned k = scheme - (pss_key ? 0x0809 : 0x0804);
        int h = k == 0 ? PK_SHA256 : k == 1 ? PK_SHA384 : PK_SHA512;
        return pss_sig(c, h, h, h, data, n, sig, slen);            /* TLS 1.3: соль = длина хэша, MGF1 с тем же хэшем */
    }
    }
    return -2;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int name_eq(const uint8_t* a, size_t an, const char* b, size_t bn) {
    if (an != bn) return 0;
    for (size_t i = 0; i < an; i++) if (lower((char)a[i]) != lower(b[i])) return 0;
    return 1;
}
static int parse_ipv4(const char* s, uint8_t ip[4]) {
    for (int k = 0; k < 4; k++) {
        int v = 0, d = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); if (++d > 3) return -1; }
        if (!d || v > 255) return -1;
        ip[k] = (uint8_t)v;
        if (k < 3) { if (*s != '.') return -1; s++; }
    }
    return *s ? -1 : 0;
}

int x509_match_host(const x509_t* c, const char* host) {
    if (!c->san) return 0;                                        /* без subjectAltName имя не принимается (CN устарел) */
    size_t hl = 0; while (host[hl]) hl++;
    uint8_t ip[4]; int is_ip = parse_ipv4(host, ip) == 0;
    der_t in = { c->san, c->san_len };
    while (in.n) {
        uint8_t t; der_t v;
        if (der_next(&in, &t, &v, 0)) return 0;
        if (is_ip) { if (t == 0x87 && v.n == 4 && v.p[0] == ip[0] && v.p[1] == ip[1] && v.p[2] == ip[2] && v.p[3] == ip[3]) return 1; continue; }
        if (t != 0x82 || !v.n) continue;
        if (name_eq(v.p, v.n, host, hl)) return 1;
        if (v.n > 2 && v.p[0] == '*' && v.p[1] == '.') {         /* *.example.com: ровно одна метка слева */
            size_t dot = 0; while (dot < hl && host[dot] != '.') dot++;
            if (dot > 0 && dot < hl && name_eq(v.p + 1, v.n - 1, host + dot, hl - dot)) return 1;
        }
    }
    return 0;
}

/* Корневой, выпустивший cur (имя издателя совпадает и подпись верна): 1 - найден. */
static int by_root(const x509_t* cur, const uint8_t* roots, size_t rl) {
    der_t in = { roots, rl };
    while (in.n) {
        der_t whole; uint8_t t;
        if (der_next(&in, &t, 0, &whole) || t != 0x30) return 0;
        x509_t r;
        if (x509_parse(whole.p, whole.n, &r)) continue;
        if (same(r.subject, r.subject_len, cur->issuer, cur->issuer_len) && x509_check_sig(cur, &r) == 0) return 1;
    }
    return 0;
}

int x509_count(const uint8_t* roots, size_t rl) {
    der_t in = { roots, rl }; int n = 0;
    while (in.n) {
        der_t whole; uint8_t t; x509_t r;
        if (der_next(&in, &t, 0, &whole) || t != 0x30) break;
        if (!x509_parse(whole.p, whole.n, &r)) n++;
    }
    return n;
}

int x509_verify_chain(const uint8_t* const* certs, const size_t* lens, int n, const uint8_t* roots, size_t roots_len,
                      const char* host, int64_t now, const char** why) {
    static x509_t chain[8];
    if (n < 1) { *why = "no certificate"; return -1; }
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) if (x509_parse(certs[i], lens[i], &chain[i])) { *why = i ? "intermediate certificate is not valid DER/X.509" : "server certificate is not valid DER/X.509"; return -1; }
    if (!x509_match_host(&chain[0], host)) { *why = "certificate is for another name (subjectAltName)"; return -2; }
    *why = 0;
    const x509_t* cur = &chain[0];
    int used[8] = { 1 };
    for (int depth = 0; depth < 8; depth++) {
        if (now && (now < cur->not_before || now > cur->not_after)) { *why = depth ? "intermediate certificate expired or not yet valid" : "certificate expired or not yet valid"; return -3; }
        if (cur->sig_alg == X509_SIG_NONE) { *why = "unsupported signature algorithm in the chain"; return -4; }
        if (by_root(cur, roots, roots_len)) { if (!now) *why = "trusted, but the clock is unknown: validity dates were not checked"; return 0; }
        int next = -1;
        for (int i = 1; i < n && next < 0; i++)
            if (!used[i] && same(chain[i].subject, chain[i].subject_len, cur->issuer, cur->issuer_len) && chain[i].is_ca &&
                x509_check_sig(cur, &chain[i]) == 0) next = i;
        if (next < 0) { *why = "issuer is not trusted (no path to a root certificate)"; return -5; }
        used[next] = 1; cur = &chain[next];
    }
    *why = "chain too long"; return -6;
}

/* ---------------- OCSP (RFC 6960) ---------------- */
static int cert_hash(const der_t* oid, const uint8_t* d, size_t n, uint8_t* out) {      /* хэш CertID: длина или 0 */
    if (oid_is(oid, "2b0e03021a")) { sha1(d, n, out); return 20; }
    int h = hash_of_oid(oid);
    if (!h) return 0;
    pk_hash(h, d, n, out);
    return h;
}

/* Издатель листа среди сертификатов присланной цепочки, затем среди корневых: 0 - найден (разобран в *iss). */
static int find_issuer(const x509_t* leaf, const uint8_t* const* certs, const size_t* lens, int n, const uint8_t* roots, size_t rl, x509_t* iss) {
    for (int i = 1; i < n; i++)
        if (!x509_parse(certs[i], lens[i], iss) && same(iss->subject, iss->subject_len, leaf->issuer, leaf->issuer_len) && x509_check_sig(leaf, iss) == 0) return 0;
    der_t in = { roots, rl };
    while (in.n) {
        der_t whole; uint8_t t;
        if (der_next(&in, &t, 0, &whole) || t != 0x30) break;
        if (!x509_parse(whole.p, whole.n, iss) && same(iss->subject, iss->subject_len, leaf->issuer, leaf->issuer_len) && x509_check_sig(leaf, iss) == 0) return 0;
    }
    return -1;
}

int x509_ocsp(const uint8_t* resp, size_t rlen, const uint8_t* const* certs, const size_t* lens, int n,
              const uint8_t* roots, size_t roots_len, int64_t now, const char** why) {
    static x509_t leaf, iss, rc, o;
    *why = 0;
    if (n < 1 || x509_parse(certs[0], lens[0], &leaf)) { *why = "OCSP: no server certificate"; return -1; }
    if (find_issuer(&leaf, certs, lens, n, roots, roots_len, &iss)) { *why = "OCSP: the issuer of the certificate is not available, response not checked"; return OCSP_NOASSURANCE; }
    der_t in = { resp, rlen }, top, st, rb, bytes, rt, basic, bs, tbs, tbsw, alg, algw, sg, extra;
    uint8_t t;
    if (der_expect(&in, 0x30, &top, 0) || der_expect(&top, 0x0a, &st, 0) || st.n != 1) { *why = "OCSP: malformed response"; return -1; }
    if (st.p[0] != 0) { *why = "OCSP: the responder answered with an error status, no revocation information"; return OCSP_NOASSURANCE; }
    if (der_expect(&top, 0xA0, &rb, 0) || der_expect(&rb, 0x30, &bytes, 0) || der_expect(&bytes, 0x06, &rt, 0) ||
        !oid_is(&rt, "2b0601050507300101") || der_expect(&bytes, 0x04, &basic, 0)) { *why = "OCSP: malformed response (not id-pkix-ocsp-basic)"; return -1; }
    if (der_expect(&basic, 0x30, &bs, 0) || der_expect(&bs, 0x30, &tbs, &tbsw) || der_expect(&bs, 0x30, &alg, &algw) ||
        der_expect(&bs, 0x03, &sg, 0) || sg.n < 2 || sg.p[0] != 0) { *why = "OCSP: malformed BasicOCSPResponse"; return -1; }
    for (size_t i = 0; i < sizeof o; i++) ((uint8_t*)&o)[i] = 0;
    o.tbs = tbsw.p; o.tbs_len = tbsw.n; o.sig = sg.p + 1; o.sig_len = sg.n - 1;
    o.sig_alg = sig_alg_of(alg, &o.pss_hash, &o.pss_mgf, &o.pss_salt);
    if (o.sig_alg == X509_SIG_NONE) { *why = "OCSP: unsupported response signature algorithm"; return -1; }
    int signed_ok = x509_check_sig(&o, &iss) == 0;                /* подписал сам издатель */
    if (!signed_ok && !der_expect(&bs, 0xA0, &extra, 0)) {        /* или уполномоченный ответчик из приложенных сертификатов */
        der_t list;
        if (!der_expect(&extra, 0x30, &list, 0)) {
            while (list.n && !signed_ok) {
                der_t cw;
                if (der_next(&list, &t, 0, &cw) || t != 0x30) break;
                if (x509_parse(cw.p, cw.n, &rc) || !rc.eku_ocsp || x509_check_sig(&rc, &iss) != 0) continue;
                if (now && (now < rc.not_before || now > rc.not_after)) continue;
                if (x509_check_sig(&o, &rc) == 0) signed_ok = 1;
            }
        }
    }
    if (!signed_ok) { *why = "OCSP: the response is not signed by the issuer or by an authorized responder"; return -1; }

    (void)der_expect(&tbs, 0xA0, &extra, 0);                     /* версия (необязательна) */
    if (der_next(&tbs, &t, 0, 0) || (t != 0xA1 && t != 0xA2)) { *why = "OCSP: malformed ResponseData (responderID)"; return -1; }
    if (der_next(&tbs, &t, 0, 0) || t != 0x18) { *why = "OCSP: malformed ResponseData (producedAt)"; return -1; }
    der_t list;
    if (der_expect(&tbs, 0x30, &list, 0)) { *why = "OCSP: malformed ResponseData (responses)"; return -1; }
    while (list.n) {
        der_t sr, cid, ha, hoid, nh, kh, ser;
        if (der_expect(&list, 0x30, &sr, 0) || der_expect(&sr, 0x30, &cid, 0)) { *why = "OCSP: malformed SingleResponse"; return -1; }
        if (der_expect(&cid, 0x30, &ha, 0) || der_expect(&ha, 0x06, &hoid, 0) || der_expect(&cid, 0x04, &nh, 0) ||
            der_expect(&cid, 0x04, &kh, 0) || der_expect(&cid, 0x02, &ser, 0)) { *why = "OCSP: malformed CertID"; return -1; }
        uint8_t d1[64], d2[64];
        int l1 = cert_hash(&hoid, leaf.issuer, leaf.issuer_len, d1), l2 = cert_hash(&hoid, iss.spk, iss.spk_len, d2);
        if (!l1 || !l2 || nh.n != (size_t)l1 || kh.n != (size_t)l2 || !same(nh.p, nh.n, d1, (size_t)l1) || !same(kh.p, kh.n, d2, (size_t)l2) ||
            !same(ser.p, ser.n, leaf.serial, leaf.serial_len)) continue;          /* ответ про другой сертификат: смотрим следующий */
        uint8_t stt; der_t sv;
        if (der_next(&sr, &stt, &sv, 0)) { *why = "OCSP: malformed certStatus"; return -1; }
        if (stt == 0xA1) { *why = "OCSP: the certificate has been revoked"; return OCSP_REVOKED; }
        int64_t tu, nu = 0;
        if (der_time(&sr, &tu)) { *why = "OCSP: malformed thisUpdate"; return -1; }
        der_t nx;
        if (!der_expect(&sr, 0xA0, &nx, 0) && der_time(&nx, &nu)) { *why = "OCSP: malformed nextUpdate"; return -1; }
        if (stt != 0x80) { *why = "OCSP: the responder does not know this certificate (status unknown)"; return OCSP_NOASSURANCE; }
        if (!now) { *why = "OCSP: response is authentic, but the clock is unknown: freshness not checked"; return OCSP_NOASSURANCE; }
        if (tu > now + 300) { *why = "OCSP: the response is from the future (thisUpdate)"; return OCSP_NOASSURANCE; }
        if (nu ? now > nu : now - tu > 7 * 86400) { *why = "OCSP: the stapled response is stale (nextUpdate passed)"; return OCSP_NOASSURANCE; }
        return OCSP_GOOD;
    }
    *why = "OCSP: the response is not about the server certificate";
    return -1;
}
