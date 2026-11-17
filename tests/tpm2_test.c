/* Хост-тест tpm2.c (А3): сборка команд NV, разбор ответов, классификация кодов, операции поверх передачи (на модели TPM tests/simtpm.h). */
#include <stdio.h>
#include <string.h>
#include "tpm2.h"
#include "simtpm.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    uint8_t b[TPM2_MAX_CMD]; uint32_t rc = 99;
    /* размеры и заголовки команд */
    CHECK(tpm2_cmd_nv_define(KIRA_NV_RELEASE_COUNTER, TPM2_NV_ATTR_COUNTER, 8, b, sizeof b) == 45);
    CHECK(b[0] == 0x80 && b[1] == 0x02 && s_get32(b + 2) == 45 && s_get32(b + 6) == 0x12A);
    CHECK(s_get32(b + 31) == KIRA_NV_RELEASE_COUNTER && s_get32(b + 37) == 0x02020012u && s_get16(b + 43) == 8);
    CHECK(tpm2_cmd_nv_define(1, 0, 8, b, 44) == 0);                      /* буфер мал */
    CHECK(tpm2_cmd_nv_increment(KIRA_NV_RELEASE_COUNTER, b, sizeof b) == 31 && s_get32(b + 6) == 0x134);
    CHECK(tpm2_cmd_nv_read(KIRA_NV_DISK_ANCHOR, 16, b, sizeof b) == 35 && s_get32(b + 6) == 0x14E && s_get16(b + 31) == 16);
    uint8_t d[16] = { 1, 2, 3 };
    CHECK(tpm2_cmd_nv_write(KIRA_NV_DISK_ANCHOR, d, 16, b, sizeof b) == 51 && s_get32(b + 6) == 0x137);
    CHECK(tpm2_cmd_nv_write(KIRA_NV_DISK_ANCHOR, d, 0, b, sizeof b) == 0);
    CHECK(tpm2_cmd_nv_write(KIRA_NV_DISK_ANCHOR, d, 16, b, 50) == 0);
    CHECK(TPM2_NV_ATTR_COUNTER == 0x02020012u && TPM2_NV_ATTR_ORDINARY == 0x02020002u);

    /* разбор ответов: строгий */
    uint8_t r[TPM2_MAX_RSP];
    CHECK(tpm2_rsp_rc(r, 5, &rc) == TPM2_E_FORMAT);
    memset(r, 0, sizeof r); r[0] = 0x80; r[1] = 0x01; r[5] = 10; CHECK(tpm2_rsp_rc(r, 10, &rc) == TPM2_OK && rc == 0);
    r[5] = 11; CHECK(tpm2_rsp_rc(r, 10, &rc) == TPM2_E_FORMAT);          /* размер не сходится */
    r[5] = 10; r[1] = 0x07; CHECK(tpm2_rsp_rc(r, 10, &rc) == TPM2_E_FORMAT);   /* неизвестный тег */
    r[1] = 1; r[9] = 0x8B; CHECK(tpm2_rsp_rc(r, 10, &rc) == TPM2_OK && rc == 0x8B);
    CHECK(tpm2_rc_class(0) == TPM2_OK && tpm2_rc_class(0x14A) == TPM2_E_EMPTY && tpm2_rc_class(0x18B) == TPM2_E_ABSENT && tpm2_rc_class(0x28B) == TPM2_E_ABSENT);
    CHECK(tpm2_rc_class(0x98E) == TPM2_E_AUTH && tpm2_rc_class(0x921) == TPM2_E_LOCKED && tpm2_rc_class(0x14C) == TPM2_E_RC);
    CHECK(tpm2_rc_class(0x84) == TPM2_E_RC);                              /* TPM2_RC_VALUE - не «нет индекса» */
    CHECK(tpm2_rc_class(0x1CB) == TPM2_E_RC);                             /* бит параметра (P) - ошибка параметра, а не дескриптора */

    /* операции поверх модели: счётчик */
    simtpm_t t; memset(&t, 0, sizeof t);
    uint64_t v = 7;
    CHECK(tpm2_nv_counter_read(sim_xfer, &t, KIRA_NV_RELEASE_COUNTER, &v, &rc) == TPM2_E_ABSENT);
    CHECK(tpm2_nv_define(sim_xfer, &t, KIRA_NV_RELEASE_COUNTER, TPM2_NV_ATTR_COUNTER, 8, &rc) == TPM2_OK);
    CHECK(tpm2_nv_define(sim_xfer, &t, KIRA_NV_RELEASE_COUNTER, TPM2_NV_ATTR_COUNTER, 8, &rc) == TPM2_E_RC && rc == 0x14C);
    CHECK(tpm2_nv_counter_read(sim_xfer, &t, KIRA_NV_RELEASE_COUNTER, &v, &rc) == TPM2_E_EMPTY);
    for (int i = 0; i < 5; i++) CHECK(tpm2_nv_increment(sim_xfer, &t, KIRA_NV_RELEASE_COUNTER, &rc) == TPM2_OK);
    CHECK(tpm2_nv_counter_read(sim_xfer, &t, KIRA_NV_RELEASE_COUNTER, &v, &rc) == TPM2_OK && v == 5);
    /* обычная ячейка */
    uint8_t w[16], g[16]; for (int i = 0; i < 16; i++) w[i] = (uint8_t)(0xA0 + i);
    CHECK(tpm2_nv_define(sim_xfer, &t, KIRA_NV_DISK_ANCHOR, TPM2_NV_ATTR_ORDINARY, 16, &rc) == TPM2_OK);
    CHECK(tpm2_nv_read(sim_xfer, &t, KIRA_NV_DISK_ANCHOR, g, 16, &rc) == TPM2_E_EMPTY);
    CHECK(tpm2_nv_write(sim_xfer, &t, KIRA_NV_DISK_ANCHOR, w, 16, &rc) == TPM2_OK);
    CHECK(tpm2_nv_read(sim_xfer, &t, KIRA_NV_DISK_ANCHOR, g, 16, &rc) == TPM2_OK && !memcmp(g, w, 16));
    CHECK(tpm2_nv_increment(sim_xfer, &t, KIRA_NV_DISK_ANCHOR, &rc) == TPM2_E_RC);   /* увеличить можно только счётчик */
    /* отказы среды */
    t.owner_auth_set = 1;
    CHECK(tpm2_nv_increment(sim_xfer, &t, KIRA_NV_RELEASE_COUNTER, &rc) == TPM2_E_AUTH && rc == 0x98E);
    t.owner_auth_set = 0; t.dead = 1;
    CHECK(tpm2_nv_counter_read(sim_xfer, &t, KIRA_NV_RELEASE_COUNTER, &v, &rc) == TPM2_E_IO);
    CHECK(tpm2_nv_read(0, 0, 1, g, 16, &rc) == TPM2_E_ARGS);
    CHECK(tpm2_nv_read(sim_xfer, &t, 1, 0, 16, &rc) == TPM2_E_ARGS);
    CHECK(tpm2_strerror(TPM2_E_AUTH)[0] != 0 && tpm2_strerror(-99)[0] != 0);
    /* ответ NV_Read с лишним или недостающим байтом отклоняется */
    uint8_t rr[TPM2_MAX_RSP]; memset(rr, 0, sizeof rr); rr[0] = 0x80; rr[1] = 0x02; s_put32(rr + 2, 29); s_put32(rr + 10, 10); rr[15] = 8;
    CHECK(tpm2_rsp_nv_data(rr, 29, g, 8) == TPM2_OK);
    CHECK(tpm2_rsp_nv_data(rr, 28, g, 8) == TPM2_E_FORMAT);
    CHECK(tpm2_rsp_nv_data(rr, 29, g, 7) == TPM2_E_FORMAT);
    s_put32(rr + 10, 11); CHECK(tpm2_rsp_nv_data(rr, 29, g, 8) == TPM2_E_FORMAT);          /* размер параметров не сходится с данными */
    s_put32(rr + 10, 10);
    uint8_t er[12]; memset(er, 0, sizeof er); er[0] = 0x80; er[1] = 0x01; er[5] = 12; er[9] = 0x8B;
    CHECK(tpm2_rsp_rc(er, 12, &rc) == TPM2_E_FORMAT);                                       /* ответ с ошибкой длиннее заголовка - не принимается */
    printf(fails ? "tpm2: FAIL (%d)\n" : "tpm2: все проверки пройдены\n", fails);
    return fails ? 1 : 0;
}
