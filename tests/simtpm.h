/* Модель TPM 2.0 для хост-тестов А3: разбирает команды NV (определение, чтение, запись, увеличение) СТРОГО по спецификации - заголовок, область
   авторизации с парольным сеансом, параметры - и ведёт состояние индексов. Модель не заменяет настоящий TPM: поведение tpm2.c на настоящем swtpm
   сверяет tests/tpm2_swtpm_test.sh, а здесь проверяются решения политики на многих состояниях и «загрузках». */
#ifndef SIMTPM_H
#define SIMTPM_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

typedef struct { uint32_t index, attrs; uint16_t size; int written; uint8_t data[32]; uint64_t counter; } simnv_t;
typedef struct {
    simnv_t nv[4]; int n;
    int owner_auth_set;          /* 1: пароль владельца задан - любая команда с пустым паролем получает отказ авторизации */
    int dead;                    /* 1: транспорт отвечает ошибкой (нет устройства) */
    unsigned cmds;               /* число принятых команд: для проверки износа NV */
    unsigned nv_writes;          /* число операций, изнашивающих NV */
} simtpm_t;

static uint32_t s_get16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t s_get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void s_put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (24 - 8 * i)); }

static size_t sim_rsp(uint8_t* r, uint32_t rc) { r[0] = 0x80; r[1] = 0x01; s_put32(r + 2, 10); s_put32(r + 6, rc); return 10; }
static simnv_t* sim_find(simtpm_t* t, uint32_t idx) { for (int i = 0; i < t->n; i++) if (t->nv[i].index == idx) return &t->nv[i]; return 0; }

/* Область авторизации обязана быть ровно «пароль, пустые nonce и пароль»: иначе модель отвечает ошибкой формата (как настоящий TPM). */
static int sim_auth_ok(const uint8_t* c, size_t off, size_t len) {
    if (off + 13 > len) return 0;
    return s_get32(c + off) == 9 && s_get32(c + off + 4) == 0x40000009u && s_get16(c + off + 8) == 0 && c[off + 10] == 1 && s_get16(c + off + 11) == 0;
}

static int sim_xfer(void* ctx, const uint8_t* c, size_t n, uint8_t* r, size_t* rl) {
    simtpm_t* t = (simtpm_t*)ctx;
    if (t->dead) return -1;
    t->cmds++;
    if (n < 10 || s_get32(c + 2) != n || s_get16(c) != 0x8002) { *rl = sim_rsp(r, 0x1E); return 0; }       /* TPM_RC_BAD_TAG */
    uint32_t cc = s_get32(c + 6);
    if (t->owner_auth_set) { *rl = sim_rsp(r, 0x98E); return 0; }                                           /* TPM_RC_AUTH_FAIL + сеанс 1 */
    if (cc == 0x12A) {                                                                                      /* NV_DefineSpace */
        if (n != 45 || s_get32(c + 10) != 0x40000001u || !sim_auth_ok(c, 14, n) || s_get16(c + 27) != 0 || s_get16(c + 29) != 14) { *rl = sim_rsp(r, 0x1C4); return 0; }
        uint32_t idx = s_get32(c + 31), attrs = s_get32(c + 37); uint16_t size = (uint16_t)s_get16(c + 43);
        if (s_get16(c + 35) != 0x000B || s_get16(c + 41) != 0) { *rl = sim_rsp(r, 0x1C4); return 0; }
        if (sim_find(t, idx)) { *rl = sim_rsp(r, 0x14C); return 0; }                                        /* TPM_RC_NV_DEFINED */
        if (t->n >= 4 || size > 32) { *rl = sim_rsp(r, 0x14B); return 0; }
        simnv_t* e = &t->nv[t->n++]; memset(e, 0, sizeof *e); e->index = idx; e->attrs = attrs; e->size = size;
        t->nv_writes++;
        *rl = sim_rsp(r, 0); return 0;
    }
    if (n < 27) { *rl = sim_rsp(r, 0x1C4); return 0; }
    uint32_t idx = s_get32(c + 14);
    simnv_t* e = sim_find(t, idx);
    if (!e) { *rl = sim_rsp(r, 0x28B); return 0; }                                                          /* TPM_RC_HANDLE, второй дескриптор */
    if (!sim_auth_ok(c, 18, n)) { *rl = sim_rsp(r, 0x1C4); return 0; }
    if (cc == 0x134) {                                                                                      /* NV_Increment */
        if (n != 31 || !(e->attrs & 0x10) || (e->attrs & 0xF0) != 0x10) { *rl = sim_rsp(r, 0x14B); return 0; }
        e->counter++; e->written = 1; t->nv_writes++;
        *rl = sim_rsp(r, 0); return 0;
    }
    if (cc == 0x14E) {                                                                                      /* NV_Read */
        if (n != 35 || s_get16(c + 33) != 0) { *rl = sim_rsp(r, 0x1C4); return 0; }
        uint32_t sz = s_get16(c + 31);
        if (!e->written) { *rl = sim_rsp(r, 0x14A); return 0; }                                             /* NV_UNINITIALIZED */
        if (sz != e->size) { *rl = sim_rsp(r, 0x146); return 0; }
        uint8_t d[32];
        if ((e->attrs & 0xF0) == 0x10) for (int i = 0; i < 8; i++) d[i] = (uint8_t)(e->counter >> (56 - 8 * i)); else memcpy(d, e->data, sz);
        r[0] = 0x80; r[1] = 0x02; s_put32(r + 6, 0);
        s_put32(r + 10, 2 + sz);                                                                            /* parameterSize */
        r[14] = (uint8_t)(sz >> 8); r[15] = (uint8_t)sz; memcpy(r + 16, d, sz);
        memset(r + 16 + sz, 0, 5);                                                                          /* ответная часть сеанса: nonce 0, атрибуты 0, hmac 0 */
        *rl = 16 + sz + 5; s_put32(r + 2, (uint32_t)*rl); return 0;
    }
    if (cc == 0x137) {                                                                                      /* NV_Write */
        uint32_t sz = s_get16(c + 31);
        if (n != 35 + sz || s_get16(c + 33 + sz) != 0 || (e->attrs & 0xF0) != 0 || sz != e->size) { *rl = sim_rsp(r, 0x14B); return 0; }
        memcpy(e->data, c + 33, sz); e->written = 1; t->nv_writes++;
        *rl = sim_rsp(r, 0); return 0;
    }
    *rl = sim_rsp(r, 0x143);                                                                                /* TPM_RC_COMMAND_CODE */
    return 0;
}
#endif
