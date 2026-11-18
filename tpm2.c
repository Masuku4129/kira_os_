#include "tpm2.h"

static void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (24 - 8 * i)); }
static uint32_t get16(const uint8_t* p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t get32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Заголовок команды: тег, размер (дописывается в конце), код. Возвращает смещение за заголовком. */
static size_t head(uint8_t* o, uint32_t tag, uint32_t cc) { put16(o, tag); put32(o + 2, 0); put32(o + 6, cc); return 10; }

/* Область авторизации: один сеанс TPM_RS_PW с пустыми nonce и паролем (9 байт + 4 байта размера). */
static size_t auth_area(uint8_t* o) {
    put32(o, 9);
    put32(o + 4, TPM2_RS_PW);
    put16(o + 8, 0);            /* nonce: пусто */
    o[10] = 0x01;               /* continueSession */
    put16(o + 11, 0);           /* пароль: пусто */
    return 13;
}

static size_t finish(uint8_t* o, size_t n) { put32(o + 2, (uint32_t)n); return n; }

size_t tpm2_cmd_nv_define(uint32_t index, uint32_t attrs, uint16_t size, uint8_t* out, size_t cap) {
    if (!out || cap < 45) return 0;
    size_t n = head(out, TPM2_ST_SESSIONS, TPM2_CC_NV_DEFINE);
    put32(out + n, TPM2_RH_OWNER); n += 4;
    n += auth_area(out + n);
    put16(out + n, 0); n += 2;                       /* TPM2B_AUTH: пароль индекса пуст (доступ - правами владельца) */
    put16(out + n, 14); n += 2;                      /* TPM2B_NV_PUBLIC: размер структуры */
    put32(out + n, index); n += 4;
    put16(out + n, TPM2_ALG_SHA256); n += 2;         /* nameAlg */
    put32(out + n, attrs); n += 4;
    put16(out + n, 0); n += 2;                       /* authPolicy: пусто */
    put16(out + n, size); n += 2;
    return finish(out, n);
}

size_t tpm2_cmd_nv_increment(uint32_t index, uint8_t* out, size_t cap) {
    if (!out || cap < 31) return 0;
    size_t n = head(out, TPM2_ST_SESSIONS, TPM2_CC_NV_INCREMENT);
    put32(out + n, TPM2_RH_OWNER); n += 4;
    put32(out + n, index); n += 4;
    n += auth_area(out + n);
    return finish(out, n);
}

size_t tpm2_cmd_nv_read(uint32_t index, uint16_t size, uint8_t* out, size_t cap) {
    if (!out || cap < 35) return 0;
    size_t n = head(out, TPM2_ST_SESSIONS, TPM2_CC_NV_READ);
    put32(out + n, TPM2_RH_OWNER); n += 4;
    put32(out + n, index); n += 4;
    n += auth_area(out + n);
    put16(out + n, size); n += 2;
    put16(out + n, 0); n += 2;                       /* смещение */
    return finish(out, n);
}

size_t tpm2_cmd_nv_write(uint32_t index, const uint8_t* data, uint16_t len, uint8_t* out, size_t cap) {
    if (!out || !data || len == 0 || len > 64 || cap < 35u + len) return 0;
    size_t n = head(out, TPM2_ST_SESSIONS, TPM2_CC_NV_WRITE);
    put32(out + n, TPM2_RH_OWNER); n += 4;
    put32(out + n, index); n += 4;
    n += auth_area(out + n);
    put16(out + n, len); n += 2;
    for (uint16_t i = 0; i < len; i++) out[n++] = data[i];
    put16(out + n, 0); n += 2;                       /* смещение */
    return finish(out, n);
}

int tpm2_rsp_rc(const uint8_t* rsp, size_t len, uint32_t* rc) {
    if (!rsp || !rc || len < 10) return TPM2_E_FORMAT;
    uint32_t tag = get16(rsp), size = get32(rsp + 2);
    if (size != len) return TPM2_E_FORMAT;
    if (tag != TPM2_ST_NO_SESSIONS && tag != TPM2_ST_SESSIONS) return TPM2_E_FORMAT;
    *rc = get32(rsp + 6);
    if (*rc != 0 && len != 10) return TPM2_E_FORMAT;     /* ответ с ошибкой - только заголовок */
    return TPM2_OK;
}

int tpm2_rsp_nv_data(const uint8_t* rsp, size_t len, uint8_t* data, uint16_t want) {
    uint32_t rc;
    int r = tpm2_rsp_rc(rsp, len, &rc);
    if (r) return r;
    if (rc != 0) return TPM2_E_RC;
    /* заголовок 10, размер параметров 4, TPM2B (размер 2 + данные), затем ответная часть сеанса: nonce 2, атрибуты 1, hmac 2 */
    if (get16(rsp) != TPM2_ST_SESSIONS || len < 10u + 4u + 2u) return TPM2_E_FORMAT;
    uint32_t psize = get32(rsp + 10);
    uint32_t dsize = get16(rsp + 14);
    if (psize != 2u + dsize || dsize != want) return TPM2_E_FORMAT;
    if ((size_t)16 + dsize + 5 != len) return TPM2_E_FORMAT;
    for (uint32_t i = 0; i < dsize; i++) data[i] = rsp[16 + i];
    return TPM2_OK;
}

int tpm2_rc_class(uint32_t rc) {
    if (rc == 0) return TPM2_OK;
    if (rc == 0x14A) return TPM2_E_EMPTY;                 /* TPM2_RC_NV_UNINITIALIZED */
    if (rc == 0x921) return TPM2_E_LOCKED;                /* TPM2_RC_LOCKOUT */
    if (rc & 0x80) {                                      /* формат 1: номер ошибки в младших 6 битах, номер параметра/сеанса/дескриптора выше */
        uint32_t e = rc & 0x3F;
        if (e == 0x0B && !(rc & 0x40) && !(rc & 0x800)) return TPM2_E_ABSENT;   /* TPM2_RC_HANDLE без признаков параметра (P) и сеанса (S): дескриптор (индекс) не найден */
        if (e == 0x0E || e == 0x09) return TPM2_E_AUTH;                                              /* TPM2_RC_AUTH_FAIL, TPM2_RC_BAD_AUTH */
    }
    if (rc == 0x149 || rc == 0x148) return TPM2_E_AUTH;   /* TPM2_RC_NV_AUTHORIZATION, TPM2_RC_NV_LOCKED */
    return TPM2_E_RC;
}

/* Один обмен: отправить, принять, достать код. Возвращает TPM2_OK только при rc == 0; иначе класс ошибки. */
static int exch(tpm2_xfer_t x, void* ctx, const uint8_t* cmd, size_t n, uint8_t* rsp, size_t* rl, uint32_t* rc) {
    uint32_t local;
    if (!rc) rc = &local;
    *rc = 0;
    if (!x || !n) return TPM2_E_ARGS;
    *rl = TPM2_MAX_RSP;
    if (x(ctx, cmd, n, rsp, rl) != 0) return TPM2_E_IO;
    int r = tpm2_rsp_rc(rsp, *rl, rc);
    if (r) return r;
    return tpm2_rc_class(*rc);
}

int tpm2_nv_define(tpm2_xfer_t x, void* ctx, uint32_t index, uint32_t attrs, uint16_t size, uint32_t* rc) {
    uint8_t c[TPM2_MAX_CMD], r[TPM2_MAX_RSP]; size_t rl;
    return exch(x, ctx, c, tpm2_cmd_nv_define(index, attrs, size, c, sizeof c), r, &rl, rc);
}

int tpm2_nv_increment(tpm2_xfer_t x, void* ctx, uint32_t index, uint32_t* rc) {
    uint8_t c[TPM2_MAX_CMD], r[TPM2_MAX_RSP]; size_t rl;
    return exch(x, ctx, c, tpm2_cmd_nv_increment(index, c, sizeof c), r, &rl, rc);
}

int tpm2_nv_write(tpm2_xfer_t x, void* ctx, uint32_t index, const uint8_t* data, uint16_t len, uint32_t* rc) {
    uint8_t c[TPM2_MAX_CMD], r[TPM2_MAX_RSP]; size_t rl;
    return exch(x, ctx, c, tpm2_cmd_nv_write(index, data, len, c, sizeof c), r, &rl, rc);
}

int tpm2_nv_read(tpm2_xfer_t x, void* ctx, uint32_t index, uint8_t* data, uint16_t len, uint32_t* rc) {
    uint8_t c[TPM2_MAX_CMD], r[TPM2_MAX_RSP]; size_t rl;
    if (!data || len == 0 || len > 32) return TPM2_E_ARGS;
    int e = exch(x, ctx, c, tpm2_cmd_nv_read(index, len, c, sizeof c), r, &rl, rc);
    if (e) return e;
    return tpm2_rsp_nv_data(r, rl, data, len);
}

int tpm2_nv_counter_read(tpm2_xfer_t x, void* ctx, uint32_t index, uint64_t* value, uint32_t* rc) {
    uint8_t b[8];
    if (!value) return TPM2_E_ARGS;
    int e = tpm2_nv_read(x, ctx, index, b, 8, rc);
    if (e) return e;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | b[i];
    *value = v;
    return TPM2_OK;
}

const char* tpm2_strerror(int err) {
    switch (err) {
    case TPM2_OK:       return "ok";
    case TPM2_E_ARGS:   return "bad arguments";
    case TPM2_E_IO:     return "TPM transport failed";
    case TPM2_E_FORMAT: return "malformed TPM response";
    case TPM2_E_RC:     return "TPM returned an error";
    case TPM2_E_ABSENT: return "NV index does not exist";
    case TPM2_E_EMPTY:  return "NV index defined but never written";
    case TPM2_E_AUTH:   return "TPM authorization refused (owner password is set?)";
    case TPM2_E_LOCKED: return "TPM is in lockout";
    default:            return "unknown error";
    }
}
