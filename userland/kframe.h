#ifndef KIRA_KFRAME_H
#define KIRA_KFRAME_H
/* Общий каркас сообщений служб Киры поверх портов: детерминированный CBOR (RFC 8949, строгий разбор), версия 1.
   Запрос:  {"v":1, "id":N, "op":"имя", "args":{...}}      (args необязателен)
   Ответ:   {"v":1, "id":N, "res":значение, "code":C}       (res необязателен; code 0 - успех, иначе отрицательная ошибка службы)
   Кто отправил, ядро знает само (port_recv возвращает pid), поэтому в сообщении нет «имени» и права по нему не выдаются.
   Лишние и повторные ключи верхнего уровня отвергаются; всё, что не по формату, - код ошибки, получатель не падает.
   Общий для ядра, программ Ring 3 и хост-тестов (tests/kframe_test.c). */
#include <stdint.h>
#include "cbor.h"

enum { KF_OK = 0, KF_E_FORMAT = 1, KF_E_VERSION = 2, KF_E_ID = 3, KF_E_OP = 4, KF_E_TYPE = 5, KF_E_KEY = 6, KF_E_CODE = 7 };
#define KF_OP_MAX 15

typedef struct {
    uint32_t id;
    char op[KF_OP_MAX + 1];          /* запрос */
    int64_t code;                    /* ответ */
    const uint8_t* body; uint32_t body_len;   /* args запроса или res ответа: один элемент CBOR внутри входного буфера; body_len 0 - нет */
} kframe_t;

/* ---- запись ---- */
/* Заголовок запроса; если has_args, сразу после вызова пишется ровно один элемент CBOR (обычно карта). */
static inline void kf_req_begin(cbor_w* w, uint32_t id, const char* op, int has_args) {
    cbor_put_map(w, has_args ? 4 : 3);
    cbor_put_cstr(w, "v"); cbor_put_uint(w, 1);
    cbor_put_cstr(w, "id"); cbor_put_uint(w, id);
    cbor_put_cstr(w, "op"); cbor_put_cstr(w, op);
    if (has_args) cbor_put_cstr(w, "args");
}
/* Ответ: begin (если has_res - затем один элемент CBOR), потом kf_resp_end с кодом. Порядок ключей - по RFC 8949 4.2.1. */
static inline void kf_resp_begin(cbor_w* w, uint32_t id, int has_res) {
    cbor_put_map(w, has_res ? 4 : 3);
    cbor_put_cstr(w, "v"); cbor_put_uint(w, 1);
    cbor_put_cstr(w, "id"); cbor_put_uint(w, id);
    if (has_res) cbor_put_cstr(w, "res");
}
static inline void kf_resp_end(cbor_w* w, int64_t code) { cbor_put_cstr(w, "code"); cbor_put_int(w, code); }
/* Готовый ответ без res одним вызовом: длина или 0, если не поместилось. */
static inline uint32_t kf_resp_code(uint8_t* out, uint32_t cap, uint32_t id, int64_t code) {
    cbor_w w; cbor_w_init(&w, out, cap);
    kf_resp_begin(&w, id, 0); kf_resp_end(&w, code);
    return w.err ? 0 : (uint32_t)w.n;
}

/* ---- разбор ---- */
/* Общая часть: перебор верхней карты. is_resp выбирает набор ключей. */
static inline int kf_parse(const uint8_t* b, uint32_t len, int is_resp, kframe_t* f) {
    f->id = 0; f->op[0] = 0; f->code = 0; f->body = 0; f->body_len = 0;
    if (cbor_check(b, len, 1) != CBOR_OK) return KF_E_FORMAT;
    cbor_r r; cbor_r_init(&r, b, len, 1);
    cbor_item it;
    if (cbor_next(&r, &it) != CBOR_OK || it.type != CBOR_MAP || it.val > 4) return KF_E_FORMAT;
    uint64_t pairs = it.val;
    int seen_v = 0, seen_id = 0, seen_op = 0, seen_code = 0, seen_body = 0;
    for (uint64_t i = 0; i < pairs; i++) {
        if (cbor_next(&r, &it) != CBOR_OK || it.type != CBOR_TEXT) return KF_E_FORMAT;
        const uint8_t* k = it.data; uint64_t kl = it.val;
        size_t vstart = r.i;
        if (kl == 1 && k[0] == 'v') {
            if (seen_v) return KF_E_KEY;
            seen_v = 1;
            if (cbor_next(&r, &it) != CBOR_OK || it.type != CBOR_UINT) return KF_E_TYPE;
            if (it.val != 1) return KF_E_VERSION;
        } else if (kl == 2 && k[0] == 'i' && k[1] == 'd') {
            if (seen_id) return KF_E_KEY;
            seen_id = 1;
            if (cbor_next(&r, &it) != CBOR_OK || it.type != CBOR_UINT) return KF_E_TYPE;
            if (it.val > 0xFFFFFFFFull) return KF_E_ID;
            f->id = (uint32_t)it.val;
        } else if (!is_resp && kl == 2 && k[0] == 'o' && k[1] == 'p') {
            if (seen_op) return KF_E_KEY;
            seen_op = 1;
            if (cbor_next(&r, &it) != CBOR_OK || it.type != CBOR_TEXT) return KF_E_TYPE;
            if (it.val == 0 || it.val > KF_OP_MAX) return KF_E_OP;
            for (uint64_t j = 0; j < it.val; j++) {
                uint8_t c = it.data[j];                               /* имя операции: латиница, цифры, . _ - (без управляющих и пробелов) */
                int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                if (!ok) return KF_E_OP;
                f->op[j] = (char)c;
            }
            f->op[it.val] = 0;
        } else if (is_resp && kl == 4 && k[0] == 'c' && k[1] == 'o' && k[2] == 'd' && k[3] == 'e') {
            if (seen_code) return KF_E_KEY;
            seen_code = 1;
            if (cbor_next(&r, &it) != CBOR_OK) return KF_E_FORMAT;
            if (it.type == CBOR_UINT) { if (it.val > 0x7FFFFFFFFFFFFFFFull) return KF_E_CODE; f->code = (int64_t)it.val; }
            else if (it.type == CBOR_NINT) { if (it.val > 0x7FFFFFFFFFFFFFFFull) return KF_E_CODE; f->code = -1 - (int64_t)it.val; }
            else return KF_E_TYPE;
        } else if ((!is_resp && kl == 4 && k[0] == 'a' && k[1] == 'r' && k[2] == 'g' && k[3] == 's') ||
                   (is_resp && kl == 3 && k[0] == 'r' && k[1] == 'e' && k[2] == 's')) {
            if (seen_body) return KF_E_KEY;
            seen_body = 1;
            if (cbor_skip(&r) != CBOR_OK) return KF_E_FORMAT;
            f->body = b + vstart; f->body_len = (uint32_t)(r.i - vstart);
        } else return KF_E_KEY;
    }
    if (r.i != len) return KF_E_FORMAT;
    if (!seen_v) return KF_E_VERSION;
    if (!seen_id) return KF_E_ID;
    if (!is_resp && !seen_op) return KF_E_OP;
    if (is_resp && !seen_code) return KF_E_CODE;
    return KF_OK;
}
static inline int kf_parse_req(const uint8_t* b, uint32_t len, kframe_t* f) { return kf_parse(b, len, 0, f); }
static inline int kf_parse_resp(const uint8_t* b, uint32_t len, kframe_t* f) { return kf_parse(b, len, 1, f); }

/* ---- аргументы: body - карта; значения проверяются по типу, чужой тип - ошибка, нет ключа - KF_ABSENT ---- */
#define KF_ABSENT 100
static inline int kf_get(const kframe_t* f, const char* key, cbor_item* it) {
    if (!f->body_len) return KF_ABSENT;
    int r = cbor_map_find(f->body, f->body_len, key, it, 0);
    if (r == CBOR_NOT_FOUND) return KF_ABSENT;
    return r == CBOR_OK ? KF_OK : KF_E_FORMAT;
}
static inline int kf_get_uint(const kframe_t* f, const char* key, uint64_t* v) {
    cbor_item it; int r = kf_get(f, key, &it);
    if (r) return r;
    if (it.type != CBOR_UINT) return KF_E_TYPE;
    *v = it.val; return KF_OK;
}
/* Текст или байты: указатель внутри входного буфера и длина. */
static inline int kf_get_text(const kframe_t* f, const char* key, const uint8_t** p, uint32_t* n) {
    cbor_item it; int r = kf_get(f, key, &it);
    if (r) return r;
    if (it.type != CBOR_TEXT || it.val > 0xFFFFFFFFull) return KF_E_TYPE;
    *p = it.data; *n = (uint32_t)it.val; return KF_OK;
}
static inline int kf_get_bytes(const kframe_t* f, const char* key, const uint8_t** p, uint32_t* n) {
    cbor_item it; int r = kf_get(f, key, &it);
    if (r) return r;
    if (it.type != CBOR_BYTES || it.val > 0xFFFFFFFFull) return KF_E_TYPE;
    *p = it.data; *n = (uint32_t)it.val; return KF_OK;
}

#endif
