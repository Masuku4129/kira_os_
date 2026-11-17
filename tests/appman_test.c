/* Хост-тест подписанного манифеста приложений (А3, appman.h): формат, вердикты, политика режима и ПОДПИСЬ: верная принимается, подмена любого байта манифеста,
   чужой домен, неизвестный, отозванный и просроченный (ротация) ключ - отказ. Подписи и манифест создают tools/kfstool и tools/kdrvsign.py
   (tests/appman_test.sh): проверяется тот же формат, который выпускает издатель, а не самодельный. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "appman.h"
#include "kdrv_state.h"
#include "kdrv_sig.h"
#include "sha256.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint8_t* slurp(const char* p, size_t* n) {
    FILE* f = fopen(p, "rb"); if (!f) { perror(p); exit(2); }
    static uint8_t buf[1 << 16]; *n = fread(buf, 1, sizeof buf, f); fclose(f);
    uint8_t* r = malloc(*n); memcpy(r, buf, *n); return r;
}

int main(int argc, char** argv) {
    if (argc < 8) { printf("использование: appman_test manifest sigA sigB sigDriver pubA pubB elf_dir\n"); return 2; }
    size_t ml, sl, bl, dl, pl;
    uint8_t* man = slurp(argv[1], &ml); uint8_t* sigA = slurp(argv[2], &sl); uint8_t* sigB = slurp(argv[3], &bl); uint8_t* sigD = slurp(argv[4], &dl);
    uint8_t* pubA = slurp(argv[5], &pl); uint8_t* pubB = slurp(argv[6], &pl);
    (void)pl;

    /* --- формат и вердикты --- */
    uint32_t serial = 0, count = 0;
    CHECK(appman_parse(man, ml, &serial, &count) == APPMAN_OK && serial == 7 && count == 3);
    char path[512]; size_t el; uint8_t d[32];
    snprintf(path, sizeof path, "%s/alpha.elf", argv[7]); uint8_t* alpha = slurp(path, &el); sha256(alpha, el, d);
    CHECK(appman_check(man, ml, "alpha", d) == TRUST_OK);
    uint8_t bad[32]; memcpy(bad, d, 32); bad[7] ^= 1;
    CHECK(appman_check(man, ml, "alpha", bad) == TRUST_TAMPERED);                 /* имя есть, образ другой */
    CHECK(appman_check(man, ml, "ghost", d) == TRUST_UNKNOWN);                    /* имени нет */
    CHECK(appman_check(man, ml, "alph", d) == TRUST_UNKNOWN && appman_check(man, ml, "alphaa", d) == TRUST_UNKNOWN);   /* имя точное, не префикс */
    CHECK(appman_check(man, ml - 1, "alpha", d) == TRUST_UNKNOWN);                /* усечённый манифест - не допуск */
    CHECK(appman_check(0, 0, "alpha", d) == TRUST_UNKNOWN && appman_check(man, ml, 0, d) == TRUST_UNKNOWN);
    /* каждый байт заголовка: порча отвергается форматом (кроме serial - он вправе быть любым) */
    for (size_t i = 0; i < APPMAN_HEADER; i++) {
        if (i >= 8 && i < 12) continue;
        uint8_t* m = malloc(ml); memcpy(m, man, ml); m[i] ^= 0x01;
        CHECK(appman_parse(m, ml, 0, 0) != APPMAN_OK); free(m);
    }
    { uint8_t* m = malloc(ml); memcpy(m, man, ml); m[8] ^= 0x01; CHECK(appman_parse(m, ml, 0, 0) == APPMAN_OK); free(m); }   /* serial меняет только подпись */
    { uint8_t* m = malloc(ml); memcpy(m, man, ml); memcpy(m + APPMAN_HEADER + APPMAN_ENTRY, m + APPMAN_HEADER, APPMAN_NAME_LEN); CHECK(appman_parse(m, ml, 0, 0) != APPMAN_OK); free(m); }   /* повтор имени */
    { uint8_t* m = malloc(ml); memcpy(m, man, ml); m[APPMAN_HEADER] = ' '; CHECK(appman_parse(m, ml, 0, 0) != APPMAN_OK); free(m); }     /* пробел в имени */
    { uint8_t* m = malloc(ml); memcpy(m, man, ml); m[APPMAN_HEADER] = 0; CHECK(appman_parse(m, ml, 0, 0) != APPMAN_OK); free(m); }     /* пустое имя */
    { uint8_t* m = malloc(ml); memcpy(m, man, ml); m[APPMAN_HEADER + 23] = 'x'; CHECK(appman_parse(m, ml, 0, 0) != APPMAN_OK); free(m); } /* имя без нуля */
    /* сборка: то, что собрал appman_build, разбирается, и наоборот */
    trust_entry_t te[2] = { { "one", { 1 } }, { "two", { 2 } } };
    uint8_t out[APPMAN_MAX_LEN];
    size_t ol = appman_build(9, te, 2, out, sizeof out);
    CHECK(ol == APPMAN_HEADER + 2 * APPMAN_ENTRY && appman_parse(out, ol, &serial, &count) == APPMAN_OK && serial == 9 && count == 2);
    CHECK(appman_build(1, te, 2, out, ol - 1) == 0 && appman_build(1, te, 0, out, sizeof out) == 0);
    trust_entry_t dup[2] = { { "one", { 1 } }, { "one", { 2 } } };
    CHECK(appman_build(1, dup, 2, out, sizeof out) == 0);
    trust_entry_t longname[1] = { { "abcdefghijklmnopqrstuvwx", { 1 } } };
    CHECK(appman_build(1, longname, 1, out, sizeof out) == 0);

    /* --- политика режима --- */
    CHECK(appman_mode(0, 0) == APPMAN_DEV);
    CHECK(appman_mode(1, 0) == APPMAN_SIGNED);        /* доверенная загрузка: подпись обязательна */
    CHECK(appman_mode(1, 1) == APPMAN_SIGNED);
    CHECK(appman_mode(0, 1) == APPMAN_SIGNED);        /* kira.signedapps в режиме разработки */

    /* --- подпись: домен APPS, ключи A (действует) и B (ротация), состояние (часы, отзыв) --- */
    kdrv_sig_key_t keys[2];
    memset(keys, 0, sizeof keys);
    memcpy(keys[0].id, "TEST-APPS-KEY-A0", 16); memcpy(keys[0].public_key, pubA, 32);
    memcpy(keys[1].id, "TEST-APPS-KEY-B0", 16); memcpy(keys[1].public_key, pubB, 32);
    const int64_t NOW = 1900000000;                   /* внутри окна [1700000000, 4102444800) из tests/appman_test.sh */
    kdrv_state_t st; kdrv_state_init(&st);
    CHECK(kdrv_state_verify(&st, KDRV_SIG_DOMAIN_APPS, man, ml, sigA, sl, keys, 2, NOW) == KDRV_SIG_OK);                 /* верная подпись */
    CHECK(kdrv_state_verify(&st, KDRV_SIG_DOMAIN_APPS, man, ml, sigB, bl, keys, 2, NOW) == KDRV_SIG_OK);                 /* новый ключ после ротации */
    for (size_t i = 0; i < ml; i++) {                                                                                      /* подмена любого байта манифеста */
        uint8_t* m = malloc(ml); memcpy(m, man, ml); m[i] ^= 0x01;
        kdrv_state_t s2; kdrv_state_init(&s2);
        if (kdrv_state_verify(&s2, KDRV_SIG_DOMAIN_APPS, m, ml, sigA, sl, keys, 2, NOW) == KDRV_SIG_OK) { printf("FAIL: подмена байта %zu не замечена\n", i); fails++; }
        free(m);
    }
    CHECK(kdrv_state_verify(&st, KDRV_SIG_DOMAIN_APPS, man, ml - 1, sigA, sl, keys, 2, NOW) != KDRV_SIG_OK);             /* усечение */
    CHECK(kdrv_state_verify(&st, KDRV_SIG_DOMAIN_APPS, man, ml, sigD, dl, keys, 2, NOW) == KDRV_SIG_E_SIGNATURE);        /* подпись драйверного назначения для манифеста не годится */
    CHECK(kdrv_state_verify(&st, KDRV_SIG_DOMAIN_DRIVER, man, ml, sigA, sl, keys, 2, NOW) == KDRV_SIG_E_SIGNATURE);      /* и наоборот */
    CHECK(kdrv_state_verify(&st, KDRV_SIG_DOMAIN_APP, man, ml, sigA, sl, keys, 2, NOW) == KDRV_SIG_E_SIGNATURE);
    CHECK(kdrv_state_verify(&st, KDRV_SIG_DOMAIN_SEAL, man, ml, sigA, sl, keys, 2, NOW) == KDRV_SIG_E_SIGNATURE);
    { uint8_t* sg = malloc(sl); memcpy(sg, sigA, sl); sg[sl - 1] ^= 1; kdrv_state_t s2; kdrv_state_init(&s2);
      CHECK(kdrv_state_verify(&s2, KDRV_SIG_DOMAIN_APPS, man, ml, sg, sl, keys, 2, NOW) == KDRV_SIG_E_SIGNATURE); free(sg); }
    CHECK(kdrv_state_verify(&st, KDRV_SIG_DOMAIN_APPS, man, ml, 0, 0, keys, 2, NOW) != KDRV_SIG_OK);                     /* без подписи */
    { kdrv_sig_key_t only[1]; memcpy(only, &keys[1], sizeof only); memcpy(only[0].id, "TEST-APPS-KEY-Z9", 16);          /* ключа издателя нет в таблице ядра */
      kdrv_state_t s2; kdrv_state_init(&s2);
      CHECK(kdrv_state_verify(&s2, KDRV_SIG_DOMAIN_APPS, man, ml, sigA, sl, only, 1, NOW) == KDRV_SIG_E_UNKNOWN_KEY); }
    /* отзыв: флагом в таблице ядра и записью в состоянии (drv revoke) - обе дороги */
    { kdrv_sig_key_t k2[2]; memcpy(k2, keys, sizeof k2); k2[1].flags = KDRV_SIG_KEY_REVOKED; kdrv_state_t s2; kdrv_state_init(&s2);
      CHECK(kdrv_state_verify(&s2, KDRV_SIG_DOMAIN_APPS, man, ml, sigB, bl, k2, 2, NOW) == KDRV_SIG_E_REVOKED);
      CHECK(kdrv_state_verify(&s2, KDRV_SIG_DOMAIN_APPS, man, ml, sigA, sl, k2, 2, NOW) == KDRV_SIG_OK); }               /* отзыв B не трогает A */
    { kdrv_state_t s2; kdrv_state_init(&s2);
      CHECK(kdrv_state_revoke_key(&s2, keys[1].id) == 1);
      CHECK(kdrv_state_verify(&s2, KDRV_SIG_DOMAIN_APPS, man, ml, sigB, bl, keys, 2, NOW) == KDRV_SIG_E_REVOKED);
      CHECK(kdrv_state_verify(&s2, KDRV_SIG_DOMAIN_APPS, man, ml, sigA, sl, keys, 2, NOW) == KDRV_SIG_OK);
      /* отзыв переживает запись состояния на диск и чтение обратно */
      uint8_t enc[KDRV_STATE_FILE_SIZE]; kdrv_state_encode(&s2, enc); kdrv_state_t s3; CHECK(kdrv_state_decode(&s3, enc, sizeof enc) == 0);
      CHECK(kdrv_state_verify(&s3, KDRV_SIG_DOMAIN_APPS, man, ml, sigB, bl, keys, 2, NOW) == KDRV_SIG_E_REVOKED); }
    /* ротация: у старого ключа A срок вышел, B продолжает работать */
    { kdrv_sig_key_t k2[2]; memcpy(k2, keys, sizeof k2); k2[0].not_after = 1800000000; kdrv_state_t s2; kdrv_state_init(&s2);
      CHECK(kdrv_state_verify(&s2, KDRV_SIG_DOMAIN_APPS, man, ml, sigA, sl, k2, 2, NOW) == KDRV_SIG_E_KEY_EXPIRED);
      CHECK(kdrv_state_verify(&s2, KDRV_SIG_DOMAIN_APPS, man, ml, sigB, bl, k2, 2, NOW) == KDRV_SIG_OK); }
    /* время: до начала действия подписи и откат часов */
    { kdrv_state_t s2; kdrv_state_init(&s2);
      CHECK(kdrv_state_verify(&s2, KDRV_SIG_DOMAIN_APPS, man, ml, sigA, sl, keys, 2, 1600000000) == KDRV_SIG_E_NOT_YET);
      CHECK(kdrv_state_verify(&s2, KDRV_SIG_DOMAIN_APPS, man, ml, sigA, sl, keys, 2, 4200000000LL) == KDRV_SIG_E_EXPIRED);
      CHECK(kdrv_state_verify(&s2, KDRV_SIG_DOMAIN_APPS, man, ml, sigA, sl, keys, 2, NOW) == KDRV_SIG_E_CLOCK); }      /* часы ушли назад дальше допуска */

    printf(fails ? "appman: FAIL (%d)\n" : "appman: все проверки пройдены\n", fails);
    return fails ? 1 : 0;
}
