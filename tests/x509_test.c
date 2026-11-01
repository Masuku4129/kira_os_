// Хост-тест X.509: цепочка корневой RSA -> промежуточный P-384 -> сервер P-256 (ci/mkchain.py, создаётся при каждом
// запуске), имена (DNS, *, IPv4), срок, недоверенный и просроченный сертификаты, подмена байта; цепочка Ed25519
// (подпись корневого RSA-PSS), сервер с ключом id-RSASSA-PSS и подписью RSA-PSS sha384; ответы OCSP (good, уполномоченный
// ответчик, revoked, unknown, устарел, подделка, чужой сертификат); разбор системной связки корневых сертификатов (если есть).
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "x509.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static size_t rd(const char* dir, const char* n, uint8_t* b, size_t cap) {
    char p[512]; if (n[0] == '/') snprintf(p, sizeof p, "%s", n); else snprintf(p, sizeof p, "%s/%s", dir, n);
    FILE* f = fopen(p, "rb"); if (!f) return 0;
    size_t k = fread(b, 1, cap, f); fclose(f); return k;
}
static uint8_t root[4096], inter[4096], leaf[4096], expired[4096], selfc[4096], big[1 << 20];

int main(int argc, char** argv) {
    const char* d = argv[1];
    size_t rl = rd(d, "root.der", root, sizeof root), il = rd(d, "inter.der", inter, sizeof inter),
           ll = rd(d, "leaf.der", leaf, sizeof leaf), el = rd(d, "expired.der", expired, sizeof expired), sl = rd(d, "self.der", selfc, sizeof selfc);
    CHECK(rl && il && ll && el && sl);
    x509_t L, I, R;
    CHECK(x509_parse(leaf, ll, &L) == 0 && L.key_type == X509_KEY_EC256 && L.sig_alg == X509_SIG_EC_SHA384 && !L.is_ca);
    CHECK(x509_parse(inter, il, &I) == 0 && I.key_type == X509_KEY_EC384 && I.sig_alg == X509_SIG_RSA_SHA256 && I.is_ca);
    CHECK(x509_parse(root, rl, &R) == 0 && R.key_type == X509_KEY_RSA && R.is_ca);
    CHECK(x509_check_sig(&L, &I) == 0 && x509_check_sig(&I, &R) == 0 && x509_check_sig(&L, &R) == -1);
    CHECK(x509_match_host(&L, "kira.test") && x509_match_host(&L, "WWW.Kira.Test") && x509_match_host(&L, "10.0.2.100"));
    CHECK(!x509_match_host(&L, "a.b.kira.test") && !x509_match_host(&L, "kira.test.evil") && !x509_match_host(&L, "10.0.2.101") && !x509_match_host(&L, "test"));

    int64_t now = L.not_before + 3600;
    const uint8_t* c2[2] = { leaf, inter }; size_t l2[2] = { ll, il };
    const char* why = 0;
    CHECK(x509_verify_chain(c2, l2, 2, root, rl, "kira.test", now, &why) == 0 && !why);
    CHECK(x509_verify_chain(c2, l2, 2, root, rl, "kira.test", 0, &why) == 0 && why);           /* часы неизвестны: сказано */
    CHECK(x509_verify_chain(c2, l2, 2, root, rl, "other.test", now, &why) < 0);
    CHECK(x509_verify_chain(c2, l2, 1, root, rl, "kira.test", now, &why) < 0);               /* нет промежуточного */
    CHECK(x509_verify_chain(c2, l2, 2, inter, il, "kira.test", now, &why) == 0);            /* доверен промежуточный - тоже путь */
    const uint8_t* ce[2] = { expired, inter }; size_t le[2] = { el, il };
    CHECK(x509_verify_chain(ce, le, 2, root, rl, "kira.test", now, &why) == -3);
    const uint8_t* cs[1] = { selfc }; size_t ls[1] = { sl };
    CHECK(x509_verify_chain(cs, ls, 1, root, rl, "kira.test", now, &why) == -5);            /* самоподписанный - не доверен */
    memcpy(big, leaf, ll); big[ll - 20] ^= 1;                                               /* испорчена подпись */
    const uint8_t* cb[2] = { big, inter };
    CHECK(x509_verify_chain(cb, l2, 2, root, rl, "kira.test", now, &why) < 0);
    memcpy(big, leaf, ll); big[60] ^= 1;                                                    /* испорчено тело */
    CHECK(x509_verify_chain(cb, l2, 2, root, rl, "kira.test", now, &why) < 0);

    /* ---- Ed25519: промежуточный (подпись корневого RSA-PSS sha256/соль 32) и сервер (подпись Ed25519) ---- */
    static uint8_t einter[4096], eleaf[4096], pleaf[4096], ob[8192];
    size_t eil = rd(d, "ed_inter.der", einter, sizeof einter), ell = rd(d, "ed_leaf.der", eleaf, sizeof eleaf), pl = rd(d, "pss_leaf.der", pleaf, sizeof pleaf);
    CHECK(eil && ell && pl);
    x509_t EI, EL, PL;
    CHECK(x509_parse(einter, eil, &EI) == 0 && EI.key_type == X509_KEY_ED25519 && EI.sig_alg == X509_SIG_RSA_PSS && EI.is_ca);
    CHECK(EI.pss_hash == 32 && EI.pss_mgf == 32 && EI.pss_salt == 32);
    CHECK(x509_parse(eleaf, ell, &EL) == 0 && EL.key_type == X509_KEY_ED25519 && EL.sig_alg == X509_SIG_ED25519);
    CHECK(x509_check_sig(&EI, &R) == 0 && x509_check_sig(&EL, &EI) == 0 && x509_check_sig(&EL, &R) == -1 && x509_check_sig(&EI, &EI) == -1);
    const uint8_t* ced[2] = { eleaf, einter }; size_t led[2] = { ell, eil };
    CHECK(x509_verify_chain(ced, led, 2, root, rl, "kira.test", EL.not_before + 3600, &why) == 0);
    memcpy(big, eleaf, ell); big[ell - 5] ^= 1;                                             /* подпись Ed25519 испорчена */
    const uint8_t* cedb[2] = { big, einter };
    CHECK(x509_verify_chain(cedb, led, 2, root, rl, "kira.test", EL.not_before + 3600, &why) < 0);
    memcpy(big, einter, eil); big[eil - 5] ^= 1;                                            /* подпись RSA-PSS испорчена */
    const uint8_t* cedc[2] = { eleaf, big }; size_t ledc[2] = { ell, eil };
    CHECK(x509_verify_chain(cedc, ledc, 2, root, rl, "kira.test", EL.not_before + 3600, &why) < 0);
    CHECK(x509_parse(pleaf, pl, &PL) == 0 && PL.key_type == X509_KEY_RSA_PSS && PL.kp_hash == 32 && PL.kp_mgf == 32 && PL.kp_salt == 32);
    CHECK(PL.sig_alg == X509_SIG_RSA_PSS && PL.pss_hash == 48 && PL.pss_mgf == 48 && PL.pss_salt == 48);
    const uint8_t* cp1[1] = { pleaf }; size_t lp1[1] = { pl };
    CHECK(x509_verify_chain(cp1, lp1, 1, root, rl, "kira.test", PL.not_before + 3600, &why) == 0);
    memcpy(big, pleaf, pl); big[pl - 7] ^= 1;
    cp1[0] = big;
    CHECK(x509_verify_chain(cp1, lp1, 1, root, rl, "kira.test", PL.not_before + 3600, &why) < 0);
    CHECK(x509_check_sig(&PL, &I) == -1);                                                  /* ключ P-384 не годится для PSS */

    /* ---- OCSP (лист основной цепочки; издатель - промежуточный) ---- */
    int64_t onow = (int64_t)time(0);                                                       /* ответы OCSP выданы относительно настоящего времени */
    static const struct { const char* f; int want; const char* what; } OC[] = {
        { "good", OCSP_GOOD, 0 }, { "deleg", OCSP_GOOD, 0 }, { "revoked", OCSP_REVOKED, "revoked" },
        { "unknown", OCSP_NOASSURANCE, "unknown" }, { "stale", OCSP_NOASSURANCE, "stale" }, { "tryagain", OCSP_NOASSURANCE, "error status" },
        { "forged", -1, "not signed" }, { "other", -1, "not about" }, { "noeku", -1, "not signed" } };
    for (unsigned i = 0; i < sizeof OC / sizeof OC[0]; i++) {
        char fn[64]; snprintf(fn, sizeof fn, "ocsp_%s.der", OC[i].f);
        size_t ol = rd(d, fn, ob, sizeof ob); const char* ow = 0;
        int r = x509_ocsp(ob, ol, c2, l2, 2, root, rl, onow, &ow);
        if (r != OC[i].want || (OC[i].what && (!ow || !strstr(ow, OC[i].what))) || (!OC[i].what && ow)) { printf("FAIL OCSP %s: %d (%s)\n", OC[i].f, r, ow ? ow : "-"); fails++; }
    }
    { size_t ol = rd(d, "ocsp_good.der", ob, sizeof ob); const char* ow = 0;
      CHECK(x509_ocsp(ob, ol, c2, l2, 2, root, rl, 0, &ow) == OCSP_NOASSURANCE && ow);                 /* часы неизвестны: свежесть не проверить */
      CHECK(x509_ocsp(ob, ol, c2, l2, 1, root, rl, onow, &ow) == OCSP_NOASSURANCE);                      /* промежуточного нет, но корневой не издатель */
      CHECK(x509_ocsp(ob, ol, c2, l2, 1, inter, il, onow, &ow) == OCSP_GOOD);                            /* издатель в хранилище */
      ob[ol - 3] ^= 1; CHECK(x509_ocsp(ob, ol, c2, l2, 2, root, rl, onow, &ow) < 0);                      /* подпись ответа испорчена */
      ob[ol - 3] ^= 1; ob[ol / 2] ^= 1; CHECK(x509_ocsp(ob, ol, c2, l2, 2, root, rl, onow, &ow) != OCSP_GOOD);   /* тело испорчено */
      CHECK(x509_ocsp(ob, 20, c2, l2, 2, root, rl, now, &ow) < 0); }                                    /* обрезан */

    int sys = -1;
    if (argc > 2) { size_t bl = rd(".", argv[2], big, sizeof big); if (bl) sys = x509_count(big, bl); }
    printf(fails ? "X509 TESTS FAILED\n" : "x509: chain RSA root -> P-384 -> P-256 (SHA-256/384), RSA-PSS and Ed25519 chains, RSASSA-PSS key, OCSP (good, delegated responder, revoked, unknown, stale, forged, other certificate), names (DNS, wildcard, IPv4), validity, untrusted/expired/tampered rejected");
    if (!fails && sys >= 0) printf("; system roots parsed: %d", sys);
    printf(fails ? "" : " - passed\n");
    return fails != 0;
}
