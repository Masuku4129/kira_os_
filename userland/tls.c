/* TLS 1.3 клиент (RFC 8446): ClientHello с X25519, ServerHello, расписание ключей HKDF, записи ChaCha20-Poly1305,
   проверка Finished сервера (подлинность рукопожатия), Finished клиента, данные приложения, KeyUpdate, close_notify. */
#include "tls.h"
#include "../crypto.h"
#include "x509.h"

enum { CT_CCS = 20, CT_ALERT = 21, CT_HS = 22, CT_APP = 23 };
enum { HS_CH = 1, HS_SH = 2, HS_NST = 4, HS_EE = 8, HS_CERT = 11, HS_CV = 15, HS_FIN = 20, HS_KU = 24 };
#define SUITE_CHACHA 0x1303

static u8 hs[40000]; static u32 hs_len;          /* собранные сообщения рукопожатия (могут идти через записи) */
static sha256_ctx_t th;                           /* хэш переписки */
static u8 certmsg[40000]; static u32 certmsg_len;  /* сообщение Certificate: цепочка сервера */
static const u8* cert_p[8]; static size_t cert_l[8]; static int cert_n;
static x509_t leaf;

static void cp(u8* d, const u8* s, u32 n) { for (u32 i = 0; i < n; i++) d[i] = s[i]; }
static u32 be16(const u8* p) { return (u32)p[0] << 8 | p[1]; }
static u32 be24(const u8* p) { return (u32)p[0] << 16 | (u32)p[1] << 8 | p[2]; }
static void th_hash(u8 out[32]) { sha256_ctx_t c = th; sha256_final(&c, out); }
static int fail(tls_t* t, const char* e) { t->err = e; t->state = -1; return -1; }

static void keys(const u8 secret[32], u8 key[32], u8 iv[12]) {
    hkdf_expand_label(secret, "key", 0, 0, key, 32);
    hkdf_expand_label(secret, "iv", 0, 0, iv, 12);
}
static void nonce(const u8 iv[12], u64 seq, u8 n[12]) {
    for (int i = 0; i < 12; i++) n[i] = iv[i];
    for (int i = 0; i < 8; i++) n[11 - i] ^= (u8)(seq >> (8 * i));
}

static int send_all(tls_t* t, const u8* p, u32 n) { return k_net_send(t->sock, p, n) == (i64)n ? 0 : -1; }

/* Одна запись с сети: тип и тело (до 16 КБ + 256). */
static int read_record(tls_t* t, u8* type, u8* body, u32* len, u32 timeout_ms) {
    for (;;) {
        if (t->in_len >= 5) {
            u32 l = be16(t->in + 3);
            if (l > 16384 + 256) return fail(t, "record too long");
            if (t->in_len >= 5 + l) {
                *type = t->in[0]; *len = l; cp(body, t->in + 5, l);
                u32 rest = t->in_len - 5 - l;
                for (u32 i = 0; i < rest; i++) t->in[i] = t->in[5 + l + i];
                t->in_len = rest;
                return 0;
            }
        }
        i64 r = k_net_recv(t->sock, t->in + t->in_len, sizeof t->in - t->in_len, timeout_ms);
        if (r == 0) return 1;                                     /* соединение закрыто */
        if (r < 0) return fail(t, "network error or timeout");
        t->in_len += (u32)r;
    }
}

static u8 rec[16384 + 512];

/* Расшифровать запись: внутренний тип и содержимое на месте. */
static int open_record(tls_t* t, const u8* hdr, u8* body, u32 len, u8* itype, u32* plen) {
    if (len < 17) return fail(t, "short encrypted record");
    u8 n[12]; nonce(t->riv, t->rseq, n);
    if (aead_chacha20_poly1305_open(t->rkey, n, hdr, 5, body, len - 16, body + len - 16, body) != 0)
        return fail(t, "record authentication failed (bad key or tampering)");
    t->rseq++;
    u32 k = len - 16;
    while (k && !body[k - 1]) k--;                                /* дополнение нулями */
    if (!k) return fail(t, "record without content type");
    *itype = body[k - 1]; *plen = k - 1;
    return 0;
}

static int write_record(tls_t* t, u8 itype, const u8* p, u32 n) {
    static u8 out[5 + 16384 + 1 + 16];
    if (n > 16384) return -1;
    u32 L = n + 1 + 16;
    out[0] = CT_APP; out[1] = 3; out[2] = 3; out[3] = (u8)(L >> 8); out[4] = (u8)L;
    cp(out + 5, p, n); out[5 + n] = itype;
    u8 nn[12]; nonce(t->wiv, t->wseq, nn);
    aead_chacha20_poly1305_seal(t->wkey, nn, out, 5, out + 5, n + 1, out + 5, out + 5 + n + 1);
    t->wseq++;
    return send_all(t, out, 5 + L);
}

/* Следующее сообщение рукопожатия: тип, тело, полная длина (с заголовком 4 байта) - в начале hs[]. */
static int next_hs(tls_t* t, int encrypted, u8* type, u32* blen, u32 timeout_ms) {
    for (;;) {
        if (hs_len >= 4) {
            u32 l = be24(hs + 1);
            if (l + 4 > sizeof hs) return fail(t, "handshake message too large");
            if (hs_len >= l + 4) { *type = hs[0]; *blen = l; return 0; }
        }
        u8 rt; u32 rl;
        int r = read_record(t, &rt, rec, &rl, timeout_ms);
        if (r) return r > 0 ? fail(t, "server closed during handshake") : -1;
        if (rt == CT_CCS) continue;                               /* совместимость (RFC 8446 D.4) - пропуск */
        if (rt == CT_ALERT) return fail(t, rl >= 2 && rec[1] == 40 ? "server alert: handshake failure (no common cipher/group?)" : "server alert during handshake");
        u8 it = rt; u32 pl = rl;
        if (encrypted) {
            if (rt != CT_APP) return fail(t, "plaintext record after keys");
            u8 hdr[5] = { rt, 3, 3, (u8)(rl >> 8), (u8)rl };
            if (open_record(t, hdr, rec, rl, &it, &pl)) return -1;
            if (it == CT_ALERT) return fail(t, "server alert during handshake");
        }
        if (it != CT_HS) return fail(t, "unexpected record during handshake");
        if (hs_len + pl > sizeof hs) return fail(t, "handshake too large");
        cp(hs + hs_len, rec, pl); hs_len += pl;
    }
}
static void hs_consume(u32 blen, int to_transcript) {
    if (to_transcript) sha256_update(&th, hs, blen + 4);
    u32 rest = hs_len - (blen + 4);
    for (u32 i = 0; i < rest; i++) hs[i] = hs[blen + 4 + i];
    hs_len = rest;
}

static u32 put16(u8* p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; return 2; }

int tls_connect(tls_t* t, i64 sock, const char* host, u32 timeout_ms, const u8* roots, u32 roots_len, i64 now) {
    for (u32 i = 0; i < sizeof *t; i++) ((u8*)t)[i] = 0;
    t->sock = sock; t->state = 1; hs_len = 0;
    sha256_init(&th);

    /* ---- ClientHello ---- */
    u8 rnd[96], priv[32], pub[32];
    i64 q = k_random(rnd, sizeof rnd);
    if (q < 0) return fail(t, "no random numbers from the kernel");
    t->weak_random = q == 0;
    cp(priv, rnd + 64, 32);
    x25519_base(pub, priv);
    static u8 ch[512];
    u32 n = 0, hl = 0; while (host[hl] && hl < 253) hl++;
    ch[n++] = HS_CH; n += 3;                                      /* длина - потом */
    ch[n++] = 3; ch[n++] = 3;
    cp(ch + n, rnd, 32); n += 32;                                 /* random */
    ch[n++] = 32; cp(ch + n, rnd + 32, 32); n += 32;              /* session id (режим совместимости) */
    n += put16(ch + n, 2); n += put16(ch + n, SUITE_CHACHA);
    ch[n++] = 1; ch[n++] = 0;                                     /* сжатие: нет */
    u32 ext = n; n += 2;
    int ip_literal = 1;                                           /* адрес IPv4: имя сервера не передаётся (RFC 6066) */
    for (u32 i = 0; i < hl; i++) if (!((host[i] >= '0' && host[i] <= '9') || host[i] == '.')) ip_literal = 0;
    if (!ip_literal) {
        n += put16(ch + n, 0x0000); n += put16(ch + n, hl + 5); n += put16(ch + n, hl + 3); ch[n++] = 0; n += put16(ch + n, hl);
        for (u32 i = 0; i < hl; i++) ch[n++] = (u8)host[i];      /* server_name */
    }
    n += put16(ch + n, 0x002b); n += put16(ch + n, 3); ch[n++] = 2; n += put16(ch + n, 0x0304);   /* supported_versions */
    n += put16(ch + n, 0x000a); n += put16(ch + n, 4); n += put16(ch + n, 2); n += put16(ch + n, 0x001d);   /* x25519 */
    static const u16 sigs[] = { 0x0403, 0x0503, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501, 0x0601 };   /* то, что умеет pk.c */
    n += put16(ch + n, 0x000d); n += put16(ch + n, 2 + 2 * 8); n += put16(ch + n, 2 * 8);
    for (int i = 0; i < 8; i++) n += put16(ch + n, sigs[i]);
    n += put16(ch + n, 0x0033); n += put16(ch + n, 2 + 4 + 32); n += put16(ch + n, 4 + 32);
    n += put16(ch + n, 0x001d); n += put16(ch + n, 32); cp(ch + n, pub, 32); n += 32;   /* key_share */
    put16(ch + ext, n - ext - 2);
    u32 body = n - 4; ch[1] = (u8)(body >> 16); ch[2] = (u8)(body >> 8); ch[3] = (u8)body;
    sha256_update(&th, ch, n);
    u8 rh[5] = { CT_HS, 3, 1, (u8)(n >> 8), (u8)n };
    if (send_all(t, rh, 5) || send_all(t, ch, n)) return fail(t, "ClientHello not sent");

    /* ---- ServerHello ---- */
    u8 ty; u32 bl;
    if (next_hs(t, 0, &ty, &bl, timeout_ms)) return -1;
    if (ty != HS_SH || bl < 38) return fail(t, "expected ServerHello");
    const u8* s = hs + 4;
    static const u8 hrr[32] = { 0xCF,0x21,0xAD,0x74,0xE5,0x9A,0x61,0x11,0xBE,0x1D,0x8C,0x02,0x1E,0x65,0xB8,0x91,
                                0xC2,0xA2,0x11,0x16,0x7A,0xBB,0x8C,0x5E,0x07,0x9E,0x09,0xE2,0xC8,0xA8,0x33,0x9C };
    if (crypto_eq(s + 2, hrr, 32)) return fail(t, "server asked for another group (HelloRetryRequest): only X25519 is offered");
    u32 o = 34, sid = s[o]; o += 1 + sid;
    if (o + 5 > bl) return fail(t, "bad ServerHello");
    t->cipher = (u16)be16(s + o); o += 3;
    if (t->cipher != SUITE_CHACHA) return fail(t, "server chose a cipher that was not offered");
    u32 el = be16(s + o); o += 2;
    int v13 = 0; const u8* spub = 0;
    for (u32 e = o; e + 4 <= o + el && e + 4 <= bl; ) {
        u32 et = be16(s + e), elen = be16(s + e + 2);
        const u8* d = s + e + 4;
        if (et == 0x002b && elen == 2 && be16(d) == 0x0304) v13 = 1;
        if (et == 0x0033 && elen == 36 && be16(d) == 0x001d && be16(d + 2) == 32) spub = d + 4;
        e += 4 + elen;
    }
    if (!v13) return fail(t, "server does not speak TLS 1.3");
    if (!spub) return fail(t, "server did not send an X25519 key share");
    u8 shared[32];
    if (x25519(shared, priv, spub)) return fail(t, "bad server key share");
    crypto_wipe(priv, sizeof priv); crypto_wipe(rnd, sizeof rnd);
    hs_consume(bl, 1);

    /* ---- расписание ключей рукопожатия ---- */
    u8 zero[32] = { 0 }, empty[32], early[32], der[32], hsec[32], h[32], csec[32], ssec[32];
    sha256("", 0, empty);
    hkdf_extract(0, 0, zero, 32, early);
    hkdf_expand_label(early, "derived", empty, 32, der, 32);
    hkdf_extract(der, 32, shared, 32, hsec);
    crypto_wipe(shared, sizeof shared);
    th_hash(h);
    hkdf_expand_label(hsec, "c hs traffic", h, 32, csec, 32);
    hkdf_expand_label(hsec, "s hs traffic", h, 32, ssec, 32);
    keys(ssec, t->rkey, t->riv); t->rseq = 0;

    /* ---- EncryptedExtensions, Certificate, CertificateVerify, Finished ---- */
    int got_fin = 0;
    while (!got_fin) {
        if (next_hs(t, 1, &ty, &bl, timeout_ms)) return -1;
        const u8* m = hs + 4;
        if (ty == HS_EE) { hs_consume(bl, 1); continue; }
        if (ty == HS_CERT) {                                      /* цепочка: контекст, список (длина 3), записи */
            if (bl < 4 || bl > sizeof certmsg) return fail(t, "bad or too large Certificate");
            cp(certmsg, m, bl); certmsg_len = bl; cert_n = 0;
            u32 c = 1 + certmsg[0];
            if (c + 3 > bl) return fail(t, "bad Certificate");
            u32 end = c + 3 + be24(certmsg + c);
            if (end > bl) return fail(t, "bad Certificate");
            c += 3;
            while (c + 3 <= end) {
                u32 cl = be24(certmsg + c);
                if (c + 3 + cl + 2 > end) return fail(t, "bad Certificate entry");
                if (cert_n < 8) { cert_p[cert_n] = certmsg + c + 3; cert_l[cert_n] = cl; cert_n++; }
                if (!t->cert_count) t->leaf_len = cl;
                t->cert_count++;
                c += 3 + cl;
                c += 2 + be16(certmsg + c);
            }
            if (!cert_n || x509_parse(cert_p[0], cert_l[0], &leaf)) return fail(t, "server certificate is not valid X.509");
            hs_consume(bl, 1); continue;
        }
        if (ty == HS_CV) {                                        /* подпись переписки ключом сертификата сервера */
            if (!cert_n || bl < 4 || 4 + be16(m + 2) != bl) return fail(t, "bad CertificateVerify");
            static u8 cvd[64 + 34 + 32];
            for (int i = 0; i < 64; i++) cvd[i] = 0x20;
            const char* ctx = "TLS 1.3, server CertificateVerify";
            for (int i = 0; i < 33; i++) cvd[64 + i] = (u8)ctx[i];
            cvd[97] = 0;
            th_hash(cvd + 98);
            int vr = x509_verify_scheme(&leaf, be16(m), cvd, sizeof cvd, m + 4, bl - 4);
            if (vr == -2) return fail(t, "server signed with an unsupported scheme");
            if (vr) return fail(t, "CertificateVerify signature is wrong (server does not own the certificate key)");
            t->cv_ok = 1;
            hs_consume(bl, 1); continue;
        }
        if (ty == HS_FIN) {
            u8 fk[32], want[32];
            hkdf_expand_label(ssec, "finished", 0, 0, fk, 32);
            th_hash(h);
            hmac_sha256(fk, 32, h, 32, want);
            if (bl != 32 || !crypto_eq(want, m, 32)) return fail(t, "server Finished is wrong (handshake was altered)");
            hs_consume(bl, 1);
            got_fin = 1; continue;
        }
        return fail(t, "unexpected handshake message");
    }
    if (!t->cert_count || !t->cv_ok) return fail(t, "server did not prove its certificate (no Certificate/CertificateVerify)");
    if (roots) {                                                  /* цепочка до корневого, имя, срок */
        const char* why = 0;
        if (x509_verify_chain(cert_p, cert_l, cert_n, roots, roots_len, host, now, &why)) return fail(t, why);
        t->verified = 1; t->why = why;
    }

    /* ---- ключи приложения (по переписке до Finished сервера включительно) ---- */
    u8 ms_[32], cap[32], sap[32];
    th_hash(h);
    hkdf_expand_label(hsec, "derived", empty, 32, der, 32);
    hkdf_extract(der, 32, zero, 32, ms_);
    hkdf_expand_label(ms_, "c ap traffic", h, 32, cap, 32);
    hkdf_expand_label(ms_, "s ap traffic", h, 32, sap, 32);

    /* ---- Finished клиента (ключом рукопожатия) ---- */
    static const u8 ccs[6] = { CT_CCS, 3, 3, 0, 1, 1 };
    if (send_all(t, ccs, 6)) return fail(t, "send failed");
    keys(csec, t->wkey, t->wiv); t->wseq = 0;
    u8 fk[32], fin[36];
    hkdf_expand_label(csec, "finished", 0, 0, fk, 32);
    fin[0] = HS_FIN; fin[1] = 0; fin[2] = 0; fin[3] = 32;
    hmac_sha256(fk, 32, h, 32, fin + 4);
    if (write_record(t, CT_HS, fin, 36)) return fail(t, "Finished not sent");

    keys(cap, t->wkey, t->wiv); t->wseq = 0;
    keys(sap, t->rkey, t->riv); t->rseq = 0;
    cp(t->rsec, sap, 32);
    crypto_wipe(hsec, 32); crypto_wipe(csec, 32); crypto_wipe(ssec, 32); crypto_wipe(ms_, 32); crypto_wipe(cap, 32); crypto_wipe(sap, 32);
    crypto_wipe(fk, 32); crypto_wipe(early, 32);
    t->state = 2;
    return 0;
}

i64 tls_send(tls_t* t, const void* p, u32 n) {
    if (t->state != 2) return -1;
    const u8* b = (const u8*)p; u32 done = 0;
    while (done < n) {
        u32 k = n - done > 16384 ? 16384 : n - done;
        if (write_record(t, CT_APP, b + done, k)) return -1;
        done += k;
    }
    return (i64)done;
}

i64 tls_recv(tls_t* t, void* p, u32 n, u32 timeout_ms) {
    u8* o = (u8*)p;
    while (t->pt_off >= t->pt_len) {
        if (t->state == 3) return 0;
        if (t->state != 2) return -1;
        u8 rt; u32 rl;
        int r = read_record(t, &rt, rec, &rl, timeout_ms);
        if (r > 0) { t->state = 3; return 0; }                    /* закрыто без close_notify */
        if (r < 0) return -1;
        if (rt == CT_CCS) continue;
        if (rt != CT_APP) { fail(t, "plaintext record after handshake"); return -1; }
        u8 hdr[5] = { rt, 3, 3, (u8)(rl >> 8), (u8)rl }, it; u32 pl;
        if (open_record(t, hdr, rec, rl, &it, &pl)) return -1;
        if (it == CT_APP) { cp(t->pt, rec, pl); t->pt_off = 0; t->pt_len = pl; break; }
        if (it == CT_ALERT) { if (pl >= 2 && rec[1] == 0) { t->state = 3; return 0; } fail(t, "server alert"); return -1; }
        if (it == CT_HS) {                                        /* после рукопожатия: билеты сессии, KeyUpdate */
            u32 c = 0;
            while (c + 4 <= pl) {
                u32 ml = be24(rec + c + 1);
                if (rec[c] == HS_KU) {                            /* новый ключ чтения; ответить своим, если просят */
                    u8 ns[32];
                    hkdf_expand_label(t->rsec, "traffic upd", 0, 0, ns, 32);
                    cp(t->rsec, ns, 32); keys(ns, t->rkey, t->riv); t->rseq = 0;
                    if (ml >= 1 && rec[c + 4] == 1) { fail(t, "server requested KeyUpdate (not supported yet)"); return -1; }
                }
                c += 4 + ml;
            }
            continue;
        }
        fail(t, "unexpected record type"); return -1;
    }
    u32 k = t->pt_len - t->pt_off < n ? t->pt_len - t->pt_off : n;
    cp(o, t->pt + t->pt_off, k); t->pt_off += k;
    return (i64)k;
}

void tls_close(tls_t* t) {
    if (t->state == 2 || t->state == 3) { static const u8 cn[2] = { 1, 0 }; (void)write_record(t, CT_ALERT, cn, 2); }
    crypto_wipe(t->rkey, 32); crypto_wipe(t->wkey, 32); crypto_wipe(t->rsec, 32);
    t->state = 0;
}
