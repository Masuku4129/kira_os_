/* Хост-тест защиты от отката (А3): политика rollback.c на модели TPM (tests/simtpm.h) - «загрузки» подряд, как в сценарии secureboot-rollback-tpm,
   откат диска и якорь, износ NV. Тот же код (rb_release_boot_check, rb_release_commit) исполняет загрузчик и ядро. */
#include <stdio.h>
#include <string.h>
#include "rollback.h"
#include "simtpm.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* Одна загрузка выпуска rel: загрузчик сверяет; если разрешил и good - ядро подтверждает «хорошую» загрузку и поднимает счётчик. */
static int boot(simtpm_t* t, uint32_t rel, uint32_t min, int require, int good, uint64_t* counter_after) {
    rb_release_result_t res;
    int r = rb_release_boot_check(t ? sim_xfer : 0, t, rel, min, require, &res);
    if (r == RB_OK && good && res.src == RB_SRC_TPM_COUNTER) {
        uint64_t v = 0;
        CHECK(rb_release_commit(sim_xfer, t, rel, &v) == TPM2_OK);
        if (counter_after) *counter_after = v;
    }
    return r;
}

int main(void) {
    /* --- чистая политика --- */
    rb_release_in_t in; rb_source_t src;
    memset(&in, 0, sizeof in);
    CHECK(rb_release_decide(1, &in, &src) == RB_OK && src == RB_SRC_STATIC_MIN);                       /* без TPM и без порога: прежнее поведение */
    in.static_min = 5; CHECK(rb_release_decide(4, &in, &src) == RB_E_ROLLBACK_MIN);                    /* порог работает и без TPM */
    CHECK(rb_release_decide(5, &in, &src) == RB_OK);
    in.require_tpm = 1; CHECK(rb_release_decide(9, &in, &src) == RB_E_NO_TPM);                          /* TPM обязателен, а счётчика нет */
    in.tpm_counter_ok = 1; in.counter = 7;
    CHECK(rb_release_decide(6, &in, &src) == RB_E_ROLLBACK_TPM);                                       /* ниже счётчика */
    CHECK(rb_release_decide(7, &in, &src) == RB_OK && src == RB_SRC_TPM_COUNTER);                      /* равный допустим: перезагрузка того же выпуска */
    CHECK(rb_release_decide(8, &in, &src) == RB_OK);
    in.counter = 0xFFFFFFFFFFull; CHECK(rb_release_decide(0xFFFFFFFFu, &in, &src) == RB_E_ROLLBACK_TPM);   /* 64-битный счётчик против 32-битного выпуска */
    CHECK(rb_release_decide(1, 0, &src) == RB_E_ARGS && rb_release_decide(1, &in, 0) == RB_E_ARGS);
    CHECK(rb_release_raise_steps(5, 0) == 5 && rb_release_raise_steps(5, 5) == 0 && rb_release_raise_steps(3, 5) == 0);
    CHECK(rb_release_raise_steps(1000, 0) == RB_MAX_RAISE);                                             /* предел износа за одну загрузку */
    CHECK(rb_release_raise_steps(0xFFFFFFFFu, 0xFFFFFFF0ull) == 15);
    CHECK(rb_release_strerror(RB_E_ROLLBACK_TPM)[0] && rb_release_strerror(-77)[0]);

    /* --- сценарий secureboot-rollback-tpm: выпуск 5 поднимает счётчик, затем выпуск 3 отвергается --- */
    simtpm_t t; memset(&t, 0, sizeof t);
    uint64_t c = 0;
    CHECK(boot(&t, 5, 0, 0, 0, &c) == RB_OK);                       /* первая загрузка: счётчик создан, но «хорошей» загрузки ещё не было */
    CHECK(t.n == 1 && t.nv[0].counter == 0);
    CHECK(boot(&t, 3, 0, 0, 0, &c) == RB_OK);                       /* пока не подтверждено, пол нулевой: выпуск 3 допустим (законный откат до подтверждения) */
    CHECK(boot(&t, 5, 0, 0, 1, &c) == RB_OK && c == 5);             /* подтверждено: счётчик 5 */
    CHECK(boot(&t, 3, 0, 0, 1, &c) == RB_E_ROLLBACK_TPM);           /* выпуск 3 с верной подписью отвергнут */
    CHECK(t.nv[0].counter == 5);                                    /* отказ счётчик не трогает */
    CHECK(boot(&t, 4, 0, 0, 1, &c) == RB_E_ROLLBACK_TPM);
    CHECK(boot(&t, 5, 0, 0, 1, &c) == RB_OK && c == 5);             /* тот же выпуск грузится снова, лишних записей в NV нет */
    unsigned w0 = t.nv_writes;
    CHECK(boot(&t, 5, 0, 0, 1, &c) == RB_OK && t.nv_writes == w0);
    CHECK(boot(&t, 6, 0, 0, 0, &c) == RB_OK && t.nv[0].counter == 5);   /* новый выпуск, но не подтверждён: пол прежний, путь назад к 5 открыт */
    CHECK(boot(&t, 5, 0, 0, 1, &c) == RB_OK);
    CHECK(boot(&t, 6, 0, 0, 1, &c) == RB_OK && c == 6);
    CHECK(boot(&t, 5, 0, 0, 1, &c) == RB_E_ROLLBACK_TPM);           /* после подтверждения 6 выпуск 5 уже откат */
    /* большой разрыв: пол растёт не выше выпуска и не больше RB_MAX_RAISE за загрузку */
    simtpm_t big; memset(&big, 0, sizeof big);
    CHECK(boot(&big, 1000, 0, 0, 1, &c) == RB_OK && c == RB_MAX_RAISE);
    CHECK(boot(&big, 999, 0, 0, 0, &c) == RB_OK);                   /* пока пол (256) ниже выпуска, выпуск 999 не отвергается: честное окно, описано в SECURE_BOOT.md */
    CHECK(big.nv[0].counter <= 1000);
    /* пол никогда не выше выпуска: законный образ не блокируется */
    CHECK(boot(&big, 1000, 0, 0, 1, &c) == RB_OK);

    /* --- нет TPM, неисправный TPM, пароль владельца --- */
    CHECK(boot(0, 3, 5, 0, 1, &c) == RB_E_ROLLBACK_MIN);            /* без TPM работает вшитый порог */
    CHECK(boot(0, 6, 5, 0, 1, &c) == RB_OK);
    { rb_release_result_t r; CHECK(rb_release_boot_check(0, 0, 6, 5, 0, &r) == RB_OK && r.src == RB_SRC_STATIC_MIN); }
    CHECK(boot(0, 6, 5, 1, 1, &c) == RB_E_NO_TPM);                  /* KIRA_REQUIRE_TPM */
    simtpm_t dead; memset(&dead, 0, sizeof dead); dead.dead = 1;
    { rb_release_result_t r; CHECK(rb_release_boot_check(sim_xfer, &dead, 6, 5, 0, &r) == RB_OK && r.src == RB_SRC_STATIC_MIN && r.tpm_err == TPM2_E_IO); }
    CHECK(rb_release_boot_check(sim_xfer, &dead, 6, 5, 1, &(rb_release_result_t){0}) == RB_E_NO_TPM);
    simtpm_t own; memset(&own, 0, sizeof own); own.owner_auth_set = 1;
    { rb_release_result_t r; CHECK(rb_release_boot_check(sim_xfer, &own, 6, 0, 0, &r) == RB_OK && r.tpm_err == TPM2_E_AUTH && r.src == RB_SRC_STATIC_MIN); }
    CHECK(boot(&own, 3, 5, 0, 0, &c) == RB_E_ROLLBACK_MIN);         /* и при недоступном счётчике порог всё равно действует */
    /* порог и счётчик действуют вместе */
    simtpm_t both; memset(&both, 0, sizeof both);
    CHECK(boot(&both, 8, 0, 0, 1, &c) == RB_OK);
    CHECK(boot(&both, 7, 5, 0, 1, &c) == RB_E_ROLLBACK_TPM);
    CHECK(boot(&both, 9, 10, 0, 1, &c) == RB_E_ROLLBACK_MIN);

    /* --- откат диска --- */
    CHECK(rb_disk_check(10, 1, 10) == RB_DISK_OK && rb_disk_check(11, 1, 10) == RB_DISK_OK);
    CHECK(rb_disk_check(9, 1, 10) == RB_DISK_ROLLBACK);
    CHECK(rb_disk_check(0, 1, 1) == RB_DISK_ROLLBACK);
    CHECK(rb_disk_check(5, 0, 0) == RB_DISK_NO_ANCHOR);
    CHECK(rb_disk_check(5, -1, 0) == RB_DISK_NO_TPM);
    CHECK(rb_disk_check(0xFFFFFFFFFFFFFFFFull, 1, 0xFFFFFFFFFFFFFFFFull) == RB_DISK_OK);
    CHECK(rb_anchor_should_write(0, 0, 3) == 1 && rb_anchor_should_write(1, 5, 6) == 1);
    CHECK(rb_anchor_should_write(1, 5, 5) == 0 && rb_anchor_should_write(1, 5, 4) == 0);               /* якорь не понижается и не пишется зря */
    CHECK(rb_anchor_should_write(-1, 0, 9) == 0);
    uint8_t a[RB_ANCHOR_LEN]; uint64_t e = 0;
    rb_anchor_encode(0x0102030405060708ull, a);
    CHECK(!memcmp(a, "KFSA", 4) && a[7] == 1 && a[8] == 1 && a[15] == 8);
    CHECK(rb_anchor_decode(a, &e) == RB_OK && e == 0x0102030405060708ull);
    for (int i = 0; i < 8; i++) { uint8_t b[RB_ANCHOR_LEN]; memcpy(b, a, sizeof b); b[i] ^= 0x40; CHECK(rb_anchor_decode(b, &e) != RB_OK); }   /* подмена заголовка */
    CHECK(rb_anchor_decode(0, &e) != RB_OK);

    printf(fails ? "rollback: FAIL (%d)\n" : "rollback: все проверки пройдены\n", fails);
    return fails ? 1 : 0;
}
