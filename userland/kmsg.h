#ifndef KIRA_KMSG_H
#define KIRA_KMSG_H
/* Сообщения портов между процессами: детерминированный CBOR (RFC 8949), получатель разбирает строго и мусор отвергает, не падая.
   Общий для программ Ring 3 (userland/ipc.c) и хост-теста (tests/kmsg_test.c). */
#include <stdint.h>
#include "cbor.h"

/* Сообщение: карта {"n": число, "op": "ping" | "pong" | "done" | "err"}; у "err" вместо "n" - "code". Ключи по правилу RFC 8949 4.2.1 (короткие раньше). */
enum { E_FORMAT = 1, E_NOOP = 2, E_TYPE = 3 };
static inline uint32_t msg_make(uint8_t* out, uint32_t cap, const char* op, uint64_t n, int with_n) {
    cbor_w w; cbor_w_init(&w, out, cap);
    int err = op[0] == 'e';
    cbor_put_map(&w, with_n ? 2 : 1);
    if (with_n && !err) { cbor_put_cstr(&w, "n"); cbor_put_uint(&w, n); }
    cbor_put_cstr(&w, "op"); cbor_put_cstr(&w, op);
    if (with_n && err) { cbor_put_cstr(&w, "code"); cbor_put_uint(&w, n); }
    return w.err ? 0 : (uint32_t)w.n;
}
/* Разбор принятого: 0 - верно (op - до 7 знаков, *n - число или 0), иначе код ошибки. Любой мусор отвергается, не роняя получателя. */
static inline int msg_parse(const uint8_t* b, uint32_t len, char op[8], uint64_t* n) {
    if (cbor_check(b, len, 1) != CBOR_OK) return E_FORMAT;
    cbor_item it;
    if (cbor_map_find(b, len, "op", &it, 0) != CBOR_OK) return E_NOOP;
    if (it.type != CBOR_TEXT || it.val == 0 || it.val > 7) return E_TYPE;
    for (uint32_t i = 0; i < it.val; i++) op[i] = (char)it.data[i];
    op[it.val] = 0;
    *n = 0;
    int f = cbor_map_find(b, len, "n", &it, 0);
    if (f == CBOR_OK) { if (it.type != CBOR_UINT) return E_TYPE; *n = it.val; }
    else if (f != CBOR_NOT_FOUND) return E_FORMAT;
    return 0;
}


#endif
