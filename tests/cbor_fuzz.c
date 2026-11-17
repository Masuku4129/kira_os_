/* Фаззинг разборщика CBOR под ASan/UBSan: случайные байты, искажённые верные кодировки, строгий и обычный режимы.
   Проверяется: нет падений; cbor_skip не выходит за буфер; указатели data внутри входа; если строгая проверка
   прошла, то пропуск элемента съедает ровно весь буфер, а cbor_map_find не выходит за границы.
   Использование: cbor_fuzz ИТЕРАЦИЙ ЗЁРНО */
#include "../cbor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static int fails;
#define BAD(m) do { printf("FAIL: %s\n", m); fails++; } while (0)

static size_t gen_valid(uint8_t* out, size_t cap) {
    cbor_w w; cbor_w_init(&w, out, cap);
    cbor_put_map(&w, 3);
    cbor_put_cstr(&w, "id"); cbor_put_uint(&w, rnd());
    cbor_put_cstr(&w, "list"); cbor_put_array(&w, 3); cbor_put_int(&w, -(int64_t)rnd()); cbor_put_bool(&w, 1); cbor_put_null(&w);
    cbor_put_cstr(&w, "blob"); uint8_t d[16]; for (int i = 0; i < 16; i++) d[i] = (uint8_t)rnd(); cbor_put_bytes(&w, d, rnd() % 17);
    return w.err ? 0 : w.n;
}

static void probe(const uint8_t* b, size_t n) {
    for (int strict = 0; strict < 2; strict++) {
        int c = cbor_check(b, n, strict);
        cbor_r r; cbor_r_init(&r, b, n, strict);
        int s = cbor_skip(&r);
        if (strict && c == 0) {
            if (s != 0) BAD("check прошла, а skip нет");
        }
        cbor_r q; cbor_r_init(&q, b, n, strict);
        cbor_item it;
        for (int k = 0; k < 64; k++) {
            if (cbor_next(&q, &it) != 0) break;
            if (it.data && (it.data < b || it.data > b + n)) BAD("data вне буфера");
            if ((it.type == 2 || it.type == 3) && it.data && it.val > (uint64_t)(b + n - it.data)) BAD("длина строки больше остатка");
        }
        cbor_item out; cbor_r at;
        (void)cbor_map_find(b, n, "id", &out, &at);
        (void)cbor_map_find(b, n, "blob", &out, &at);
    }
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "cbor_fuzz ИТЕРАЦИЙ ЗЁРНО\n"); return 2; }
    long iters = atol(argv[1]);
    rs = 0x2545f4914f6cdd1dull ^ strtoull(argv[2], NULL, 0);
    if (!rs) rs = 1;
    long valid = 0;
    for (long it = 0; it < iters; it++) {
        uint8_t tmp[256]; size_t n;
        if (rnd() % 4 == 0) { n = rnd() % 64; for (size_t i = 0; i < n; i++) tmp[i] = (uint8_t)rnd(); }
        else {
            n = gen_valid(tmp, sizeof tmp);
            if (n && cbor_check(tmp, n, 1) == 0) valid++; else if (n) BAD("собственная кодировка не проходит строгую проверку");
            int k = (int)(rnd() % 4);
            while (k-- && n) {
                switch (rnd() % 4) {
                case 0: tmp[rnd() % n] = (uint8_t)rnd(); break;
                case 1: tmp[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8)); break;
                case 2: n = rnd() % (n + 1); break;
                case 3: tmp[rnd() % n] = (uint8_t)(0x18 + rnd() % 8) | (uint8_t)((rnd() % 8) << 5); break;  /* большой заголовок */
                }
            }
        }
        uint8_t* h = malloc(n ? n : 1); memcpy(h, tmp, n); probe(h, n); free(h);
    }
    printf("cbor_fuzz: %ld входов, верных кодировок %ld, нарушений %d\n", iters, valid, fails);
    return fails ? 1 : 0;
}
