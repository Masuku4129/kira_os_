// Хост-тест X.509: цепочка корневой RSA -> промежуточный P-384 -> сервер P-256 (ci/mkchain.py, создаётся при каждом
// запуске), имена (DNS, *, IPv4), срок, недоверенный и просроченный сертификаты, подмена байта; разбор системной
// связки корневых сертификатов (если есть).
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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

    int sys = -1;
    if (argc > 2) { size_t bl = rd(".", argv[2], big, sizeof big); if (bl) sys = x509_count(big, bl); }
    printf(fails ? "X509 TESTS FAILED\n" : "x509: chain RSA root -> P-384 -> P-256 (SHA-256/384), names (DNS, wildcard, IPv4), validity, untrusted/expired/tampered rejected");
    if (!fails && sys >= 0) printf("; system roots parsed: %d", sys);
    printf(fails ? "" : " - passed\n");
    return fails != 0;
}
