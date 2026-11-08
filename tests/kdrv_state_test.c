/* Хост-тест защиты драйверов от отката (kdrv_state.h): версия внутри подписанного пакета, пол версий, повреждённый файл
   состояния, отозванный ключ, часы назад, ротация ключа. Проверяется kdrv_admit - ровно та функция, что решает в ядре. */
#include "kdrv_state.h"
#include "immune.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kdrv_state_vectors.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

#define T0 1750000000LL            /* внутри окна подписей [1700000000, 1800000000) */
#define SIG_END 1800000000LL

static kdrv_sig_key_t key_a(uint64_t not_after) {
    kdrv_sig_key_t k = { "TEST-ED25519-001", {0}, 0, not_after };
    memcpy(k.public_key, vec_pub_A, 32);
    return k;
}
static kdrv_sig_key_t key_b(void) {
    kdrv_sig_key_t k = { "TEST-ED25519-002", {0}, 0, 0 };
    memcpy(k.public_key, vec_pub_B, 32);
    return k;
}

/* Допуск пакета из вектора; sig == 0 - подписи нет. Размеры пакета и подписи берутся из самих массивов вектора. */
static int admit(kdrv_state_t* st, const uint8_t* pkg, size_t pkg_len, const uint8_t* sig, int required,
                 const kdrv_sig_key_t* keys, uint32_t nk, int64_t now, kdrv_header_t* h, int* au, int* se) {
    return kdrv_admit(st, pkg, pkg_len, sig, sig ? 144 : 0, required, keys, nk, now, h, au, se);
}
#define ADMIT(st, pkg, sig, req, keys, nk, now, hv, au, se) admit((st), (pkg), sizeof(pkg), (sig), (req), (keys), (nk), (now), (hv), (au), (se))

static void test_normal_update(void) {
    kdrv_state_t st; kdrv_state_init(&st);
    kdrv_sig_key_t keys[1] = { key_a(0) };
    kdrv_header_t h; int au, se;
    CHECK(ADMIT(&st, vec_pkg_v5, vec_sig_v5_A, 1, keys, 1, T0, &h, &au, &se) == KDRV_OK);
    CHECK(au == KDRV_AUTH_PUBLISHER && se == KDRV_SIG_OK && h.version == 5);
    CHECK(kdrv_state_floor_of(&st, "tdrv") == 0);                 /* пока драйвер не проработал, пол не растёт */
    CHECK(kdrv_state_raise_floor(&st, "tdrv", 5) == 1);           /* ядро делает это после успешного запуска */
    CHECK(ADMIT(&st, vec_pkg_v5, vec_sig_v5_A, 1, keys, 1, T0, &h, &au, &se) == KDRV_OK);   /* та же версия допустима */
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, T0, &h, &au, &se) == KDRV_OK);   /* обновление */
    CHECK(h.version == 6 && kdrv_state_floor_of(&st, "tdrv") == 5);
    CHECK(kdrv_state_raise_floor(&st, "tdrv", 6) == 1 && kdrv_state_floor_of(&st, "tdrv") == 6);
    CHECK(kdrv_state_raise_floor(&st, "tdrv", 6) == 0);
}

static void test_rollback(void) {
    kdrv_state_t st; kdrv_state_init(&st);
    kdrv_sig_key_t keys[1] = { key_a(0) };
    kdrv_header_t h; int au, se;
    CHECK(kdrv_state_raise_floor(&st, "tdrv", 6) == 1);
    CHECK(ADMIT(&st, vec_pkg_v5, vec_sig_v5_A, 1, keys, 1, T0, &h, &au, &se) == KDRV_E_ROLLBACK);   /* подпись верна, версия ниже пола */
    CHECK(se == KDRV_SIG_OK && au == KDRV_AUTH_LOCAL && h.version == 5);
    CHECK(ADMIT(&st, vec_pkg_v4, vec_sig_v4_A, 1, keys, 1, T0, &h, &au, &se) == KDRV_E_ROLLBACK);
    CHECK(ADMIT(&st, vec_pkg_v7, vec_sig_v7_A, 1, keys, 1, T0, &h, &au, &se) == KDRV_OK);
    /* локальный (неподписанный) пакет тоже подчиняется полу */
    CHECK(ADMIT(&st, vec_pkg_v5, (const uint8_t*)0, 0, keys, 1, T0, &h, &au, &se) == KDRV_E_ROLLBACK);
    CHECK(ADMIT(&st, vec_pkg_v6, (const uint8_t*)0, 0, keys, 1, T0, &h, &au, &se) == KDRV_OK && au == KDRV_AUTH_LOCAL);
    /* подпись обязательна, но её нет */
    CHECK(ADMIT(&st, vec_pkg_v6, (const uint8_t*)0, 1, keys, 1, T0, &h, &au, &se) == KDRV_E_SIGNATURE && se == KDRV_SIG_E_MISSING);
    /* понизить пол нельзя; чужое имя пол не затрагивает */
    CHECK(kdrv_state_raise_floor(&st, "tdrv", 2) == 0 && kdrv_state_floor_of(&st, "tdrv") == 6);
    CHECK(kdrv_state_floor_of(&st, "other") == 0);
    CHECK(kdrv_state_raise_floor(&st, "Bad/Name", 1) == KDRV_STATE_E_ARG);
    CHECK(strcmp(kdrv_strerror(KDRV_E_ROLLBACK), "?") != 0 && strcmp(kdrv_strerror(KDRV_E_STATE), "?") != 0);
}

static void test_version_is_signed(void) {
    kdrv_state_t st; kdrv_state_init(&st);
    kdrv_sig_key_t keys[1] = { key_a(0) };
    kdrv_header_t h; int au, se;
    CHECK(kdrv_state_raise_floor(&st, "tdrv", 6) == 1);
    uint8_t forged[sizeof vec_pkg_v5];
    memcpy(forged, vec_pkg_v5, sizeof forged);
    CHECK(forged[24] == 5);                                      /* kdrv_header_t.version: смещение 24 */
    forged[24] = 6;                                              /* v5 выдаётся за v6, чтобы пройти пол */
    CHECK(kdrv_admit(&st, forged, sizeof forged, vec_sig_v5_A, sizeof vec_sig_v5_A, 1, keys, 1, T0, &h, &au, &se) == KDRV_E_SIGNATURE);
    CHECK(se == KDRV_SIG_E_HASH);                                /* версия входит в SHA-256 пакета, а он - в подписанное сообщение */
    CHECK(kdrv_admit(&st, forged, sizeof forged, vec_sig_v6_A, sizeof vec_sig_v6_A, 1, keys, 1, T0, &h, &au, &se) == KDRV_E_SIGNATURE);
    CHECK(se == KDRV_SIG_E_HASH);                                /* и чужая подпись другой версии не подходит */
}

static void rehash(uint8_t* b) { sha256(b, 920, b + 920); }

static void test_corrupt_state(void) {
    kdrv_state_t st, good; kdrv_state_init(&good);
    CHECK(kdrv_state_raise_floor(&good, "tdrv", 6) == 1 && kdrv_state_raise_floor(&good, "net-a", 3) == 1);
    uint8_t key[16] = "TEST-ED25519-001";
    CHECK(kdrv_state_revoke_key(&good, key) == 1);
    good.max_time = (uint64_t)T0;
    uint8_t buf[KDRV_STATE_FILE_SIZE];
    kdrv_state_encode(&good, buf);
    CHECK(kdrv_state_decode(&st, buf, sizeof buf) == 0 && st.status == KDRV_STATE_OK);
    CHECK(st.max_time == (uint64_t)T0 && st.nfloor == 2 && st.nrevoked == 1);
    CHECK(kdrv_state_floor_of(&st, "tdrv") == 6 && kdrv_state_floor_of(&st, "net-a") == 3 && kdrv_state_is_revoked(&st, key));
    uint8_t again[KDRV_STATE_FILE_SIZE];
    kdrv_state_encode(&st, again);
    CHECK(memcmp(buf, again, sizeof buf) == 0);

    for (unsigned i = 0; i < sizeof buf; i++) {                  /* любой испорченный байт - отказ, а не сброс */
        uint8_t bad[sizeof buf]; memcpy(bad, buf, sizeof bad); bad[i] ^= 0x40;
        CHECK(kdrv_state_decode(&st, bad, sizeof bad) == KDRV_STATE_E_CORRUPT);
        CHECK(st.status == KDRV_STATE_CORRUPT && st.nfloor == 0 && st.nrevoked == 0);
    }
    CHECK(kdrv_state_decode(&st, buf, sizeof buf - 1) == KDRV_STATE_E_CORRUPT);
    uint8_t longer[sizeof buf + 1]; memcpy(longer, buf, sizeof buf); longer[sizeof buf] = 0;
    CHECK(kdrv_state_decode(&st, longer, sizeof longer) == KDRV_STATE_E_CORRUPT);
    CHECK(kdrv_state_decode(&st, buf, 0) == KDRV_STATE_E_CORRUPT);
    CHECK(kdrv_state_decode(&st, 0, sizeof buf) == KDRV_STATE_E_CORRUPT);

    /* содержимое испорчено так, что хеш сходится: проверяются сами поля */
    uint8_t bad[sizeof buf];
    memcpy(bad, buf, sizeof bad); bad[16] = 33; rehash(bad);                       /* слишком много полов */
    CHECK(kdrv_state_decode(&st, bad, sizeof bad) == KDRV_STATE_E_CORRUPT);
    memcpy(bad, buf, sizeof bad); bad[24] = 'T'; rehash(bad);                      /* заглавная буква в имени */
    CHECK(kdrv_state_decode(&st, bad, sizeof bad) == KDRV_STATE_E_CORRUPT);
    memcpy(bad, buf, sizeof bad); memcpy(bad + 44, bad + 24, 16); rehash(bad);     /* два пола с одним именем */
    CHECK(kdrv_state_decode(&st, bad, sizeof bad) == KDRV_STATE_E_CORRUPT);
    memcpy(bad, buf, sizeof bad); bad[24 + 5 * 20] = 1; rehash(bad);               /* мусор в неиспользуемой записи */
    CHECK(kdrv_state_decode(&st, bad, sizeof bad) == KDRV_STATE_E_CORRUPT);
    memcpy(bad, buf, sizeof bad); bad[15] = 0x80; rehash(bad);                     /* время вне диапазона */
    CHECK(kdrv_state_decode(&st, bad, sizeof bad) == KDRV_STATE_E_CORRUPT);
    memcpy(bad, buf, sizeof bad); bad[4] = 2; rehash(bad);                         /* неизвестный формат */
    CHECK(kdrv_state_decode(&st, bad, sizeof bad) == KDRV_STATE_E_CORRUPT);

    /* повреждённое состояние: пакеты отклоняются с KDRV_E_STATE, пол не «забывается» */
    kdrv_sig_key_t keys[1] = { key_a(0) };
    kdrv_header_t h; int au, se;
    CHECK(kdrv_state_decode(&st, bad, sizeof bad) == KDRV_STATE_E_CORRUPT);
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, T0, &h, &au, &se) == KDRV_E_STATE);
    CHECK(ADMIT(&st, vec_pkg_v6, (const uint8_t*)0, 0, keys, 1, T0, &h, &au, &se) == KDRV_E_STATE);   /* и локальные тоже */
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, T0, &h, &au, &se) == KDRV_E_STATE);        /* повторно: сам не лечится */
    CHECK(st.status == KDRV_STATE_CORRUPT);
    kdrv_state_mark_io_error(&st);
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, T0, &h, &au, &se) == KDRV_E_STATE && st.status == KDRV_STATE_IO);
    kdrv_state_init(&st);                                        /* явный сброс (drv state-reset) - единственный выход */
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, T0, &h, &au, &se) == KDRV_OK);
}

static void test_revocation(void) {
    kdrv_state_t st; kdrv_state_init(&st);
    kdrv_sig_key_t keys[2] = { key_a(0), key_b() };
    kdrv_header_t h; int au, se;
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 2, T0, &h, &au, &se) == KDRV_OK);
    CHECK(kdrv_state_revoke_key(&st, keys[0].id) == 1 && kdrv_state_revoke_key(&st, keys[0].id) == 0);
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 2, T0, &h, &au, &se) == KDRV_E_SIGNATURE && se == KDRV_SIG_E_REVOKED);
    CHECK(au == KDRV_AUTH_LOCAL);
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_B, 1, keys, 2, T0, &h, &au, &se) == KDRV_OK);          /* второй ключ не затронут */
    CHECK(keys[0].flags == 0);                                   /* отзыв из файла не меняет вшитую таблицу */
    kdrv_state_t fresh; kdrv_state_init(&fresh);                 /* отзыв, вшитый в ядро (флаг в kdrv_trust.h) */
    keys[0].flags = KDRV_SIG_KEY_REVOKED;
    CHECK(ADMIT(&fresh, vec_pkg_v6, vec_sig_v6_A, 1, keys, 2, T0, &h, &au, &se) == KDRV_E_SIGNATURE && se == KDRV_SIG_E_REVOKED);
    uint8_t id[16], zero[16] = {0};
    CHECK(kdrv_state_revoke_key(&fresh, zero) == KDRV_STATE_E_ARG);
    for (unsigned i = 0; i < KDRV_STATE_MAX_REVOKED; i++) { memset(id, 'a' + (int)i, sizeof id); CHECK(kdrv_state_revoke_key(&fresh, id) == 1); }
    memset(id, 'z', sizeof id);
    CHECK(kdrv_state_revoke_key(&fresh, id) == KDRV_STATE_E_FULL);
    uint8_t buf[KDRV_STATE_FILE_SIZE]; kdrv_state_t back;
    kdrv_state_encode(&fresh, buf);
    CHECK(kdrv_state_decode(&back, buf, sizeof buf) == 0 && back.nrevoked == KDRV_STATE_MAX_REVOKED);
    for (unsigned i = 0; i < 32; i++) {                          /* таблица полов */
        char n[8]; snprintf(n, sizeof n, "d%u", i);
        int r = kdrv_state_raise_floor(&fresh, n, 1);
        CHECK(i < KDRV_STATE_MAX_FLOORS ? r == 1 : r == KDRV_STATE_E_FULL);
    }
    CHECK(kdrv_state_raise_floor(&fresh, "extra", 1) == KDRV_STATE_E_FULL);
    CHECK(kdrv_state_raise_floor(&fresh, "d3", 9) == 1);         /* существующее имя поднимается и при полной таблице */
}

static void test_clock(void) {
    kdrv_state_t st; kdrv_state_init(&st);
    kdrv_sig_key_t keys[1] = { key_a(0) };
    kdrv_header_t h; int au, se;
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, T0, &h, &au, &se) == KDRV_OK);
    CHECK(st.max_time == (uint64_t)T0 && st.dirty);               /* время запомнено и просит записи */
    st.dirty = 0; st.persisted_time = st.max_time;
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, T0 + 60, &h, &au, &se) == KDRV_OK);
    CHECK(!st.dirty && st.max_time == (uint64_t)T0 + 60);        /* меньше часа - диск не трогаем */
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, T0 + KDRV_TIME_PERSIST_STEP, &h, &au, &se) == KDRV_OK && st.dirty);

    uint64_t seen = st.max_time;
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, (int64_t)seen - 2 * 86400, &h, &au, &se) == KDRV_E_SIGNATURE);
    CHECK(se == KDRV_SIG_E_CLOCK && st.max_time == seen);        /* отказ не портит максимум */
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, (int64_t)seen - KDRV_CLOCK_TOLERANCE - 1, &h, &au, &se) == KDRV_E_SIGNATURE && se == KDRV_SIG_E_CLOCK);
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, (int64_t)seen - KDRV_CLOCK_TOLERANCE, &h, &au, &se) == KDRV_OK);   /* граница допуска */
    CHECK(ADMIT(&st, vec_pkg_v6, (const uint8_t*)0, 0, keys, 1, (int64_t)seen - 100 * 86400, &h, &au, &se) == KDRV_OK);   /* без подписи время не нужно */
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, 0, &h, &au, &se) == KDRV_E_SIGNATURE && se == KDRV_SIG_E_TIME);   /* часов нет */

    /* атака: подпись истекла, часы возвращают назад, чтобы она ожила */
    kdrv_state_init(&st);
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, SIG_END - 1, &h, &au, &se) == KDRV_OK);
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, SIG_END + 100, &h, &au, &se) == KDRV_E_SIGNATURE && se == KDRV_SIG_E_EXPIRED);
    CHECK(st.max_time == (uint64_t)SIG_END + 100);               /* отказное время тоже запомнено */
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, SIG_END - 10 * 86400, &h, &au, &se) == KDRV_E_SIGNATURE && se == KDRV_SIG_E_CLOCK);
    /* внутри допуска подпись не оживает: время считается не меньше максимума */
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, SIG_END + 100 - 3600, &h, &au, &se) == KDRV_E_SIGNATURE && se == KDRV_SIG_E_EXPIRED);
    /* перезагрузка: максимум пережил запись и чтение файла */
    uint8_t buf[KDRV_STATE_FILE_SIZE]; kdrv_state_t reloaded;
    kdrv_state_encode(&st, buf);
    CHECK(kdrv_state_decode(&reloaded, buf, sizeof buf) == 0);
    CHECK(ADMIT(&reloaded, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, SIG_END - 10 * 86400, &h, &au, &se) == KDRV_E_SIGNATURE && se == KDRV_SIG_E_CLOCK);
    /* часы были заведены вперёд по ошибке: явная команда time-reset */
    kdrv_state_reset_time(&reloaded, (uint64_t)T0);
    CHECK(reloaded.dirty && ADMIT(&reloaded, vec_pkg_v6, vec_sig_v6_A, 1, keys, 1, T0, &h, &au, &se) == KDRV_OK);
    CHECK(strcmp(kdrv_sig_strerror(KDRV_SIG_E_CLOCK), kdrv_sig_strerror(-99)) != 0);
}

static void test_rotation(void) {
    const int64_t rot = T0 + 1000;
    kdrv_sig_key_t keys[2] = { key_a((uint64_t)rot), key_b() };
    kdrv_header_t h; int au, se;
    kdrv_state_t st; kdrv_state_init(&st);
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 2, rot - 1, &h, &au, &se) == KDRV_OK);     /* до даты старый ключ работает */
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_B, 1, keys, 2, rot - 1, &h, &au, &se) == KDRV_OK);     /* новый - сразу */
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 2, rot, &h, &au, &se) == KDRV_E_SIGNATURE && se == KDRV_SIG_E_KEY_EXPIRED);
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_B, 1, keys, 2, rot, &h, &au, &se) == KDRV_OK);
    /* возврат часов в допуске не воскрешает старый ключ */
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 2, rot - 100, &h, &au, &se) == KDRV_E_SIGNATURE && se == KDRV_SIG_E_KEY_EXPIRED);
    /* а дальше допуска - часы назад */
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_A, 1, keys, 2, rot - 3 * 86400, &h, &au, &se) == KDRV_E_SIGNATURE && se == KDRV_SIG_E_CLOCK);
    /* пол и отзыв работают поверх ротации */
    CHECK(kdrv_state_raise_floor(&st, "tdrv", 7) == 1);
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_B, 1, keys, 2, rot, &h, &au, &se) == KDRV_E_ROLLBACK);
    CHECK(kdrv_state_revoke_key(&st, keys[1].id) == 1);
    CHECK(ADMIT(&st, vec_pkg_v7, vec_sig_v7_A, 1, keys, 2, rot, &h, &au, &se) == KDRV_E_SIGNATURE);
    CHECK(ADMIT(&st, vec_pkg_v6, vec_sig_v6_B, 1, keys, 2, rot, &h, &au, &se) == KDRV_E_SIGNATURE && se == KDRV_SIG_E_REVOKED);
}

int main(void) {
    test_normal_update();
    test_rollback();
    test_version_is_signed();
    test_corrupt_state();
    test_revocation();
    test_clock();
    test_rotation();
    puts("kdrv_state: version floor, rollback, corrupt state, revocation, clock rollback and key rotation passed");
    return 0;
}
