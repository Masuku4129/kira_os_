/* CBOR: векторы из приложения A RFC 8949, детерминированность, строгий режим, границы, случайные данные (ASAN/UBSAN). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../cbor.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, m); fails++; } } while (0)

static size_t unhex(const char* h, uint8_t* o) { size_t n = 0; for (; h[0] && h[1]; h += 2) { unsigned v; sscanf(h, "%2x", &v); o[n++] = (uint8_t)v; } return n; }
static void same(cbor_w* w, const char* hex, const char* what) {
    uint8_t e[64]; size_t n = unhex(hex, e);
    if (w->err || w->n != n || memcmp(w->p, e, n)) { printf("FAIL: %s (err %d, %zu bytes)\n", what, w->err, w->n); fails++; }
}

int main(void) {
    uint8_t buf[256]; cbor_w w;
    static const struct { uint64_t v; const char* hex; } U[] = {
        {0, "00"}, {1, "01"}, {10, "0a"}, {23, "17"}, {24, "1818"}, {25, "1819"}, {100, "1864"}, {1000, "1903e8"},
        {1000000, "1a000f4240"}, {1000000000000ULL, "1b000000e8d4a51000"}, {18446744073709551615ULL, "1bffffffffffffffff"},
    };
    for (unsigned i = 0; i < sizeof U / sizeof U[0]; i++) { cbor_w_init(&w, buf, sizeof buf); cbor_put_uint(&w, U[i].v); same(&w, U[i].hex, "uint"); }
    static const struct { int64_t v; const char* hex; } N[] = { {-1, "20"}, {-10, "29"}, {-100, "3863"}, {-1000, "3903e7"}, {INT64_MIN, "3b7fffffffffffffff"} };
    for (unsigned i = 0; i < sizeof N / sizeof N[0]; i++) { cbor_w_init(&w, buf, sizeof buf); cbor_put_int(&w, N[i].v); same(&w, N[i].hex, "int"); }

    cbor_w_init(&w, buf, sizeof buf); cbor_put_cstr(&w, ""); same(&w, "60", "text empty");
    cbor_w_init(&w, buf, sizeof buf); cbor_put_cstr(&w, "a"); same(&w, "6161", "text a");
    cbor_w_init(&w, buf, sizeof buf); cbor_put_cstr(&w, "IETF"); same(&w, "6449455446", "text IETF");
    cbor_w_init(&w, buf, sizeof buf); cbor_put_cstr(&w, "\"\\"); same(&w, "62225c", "text quote");
    cbor_w_init(&w, buf, sizeof buf); cbor_put_cstr(&w, "\xc3\xbc"); same(&w, "62c3bc", "text u-umlaut");
    cbor_w_init(&w, buf, sizeof buf); cbor_put_cstr(&w, "\xe6\xb0\xb4"); same(&w, "63e6b0b4", "text water");
    { const uint8_t d[4] = {1, 2, 3, 4}; cbor_w_init(&w, buf, sizeof buf); cbor_put_bytes(&w, d, 4); same(&w, "4401020304", "bytes"); }
    cbor_w_init(&w, buf, sizeof buf); cbor_put_array(&w, 0); same(&w, "80", "empty array");
    cbor_w_init(&w, buf, sizeof buf); cbor_put_array(&w, 3); cbor_put_uint(&w, 1); cbor_put_uint(&w, 2); cbor_put_uint(&w, 3); same(&w, "83010203", "[1,2,3]");
    cbor_w_init(&w, buf, sizeof buf); cbor_put_array(&w, 3); cbor_put_uint(&w, 1); cbor_put_array(&w, 2); cbor_put_uint(&w, 2); cbor_put_uint(&w, 3);
    cbor_put_array(&w, 2); cbor_put_uint(&w, 4); cbor_put_uint(&w, 5); same(&w, "8301820203820405", "[1,[2,3],[4,5]]");
    cbor_w_init(&w, buf, sizeof buf); cbor_put_map(&w, 0); same(&w, "a0", "empty map");
    cbor_w_init(&w, buf, sizeof buf); cbor_put_map(&w, 2); cbor_put_uint(&w, 1); cbor_put_uint(&w, 2); cbor_put_uint(&w, 3); cbor_put_uint(&w, 4); same(&w, "a201020304", "{1:2,3:4}");
    cbor_w_init(&w, buf, sizeof buf); cbor_put_map(&w, 2); cbor_put_cstr(&w, "a"); cbor_put_uint(&w, 1); cbor_put_cstr(&w, "b");
    cbor_put_array(&w, 2); cbor_put_uint(&w, 2); cbor_put_uint(&w, 3); same(&w, "a26161016162820203", "{a:1,b:[2,3]}");
    cbor_w_init(&w, buf, sizeof buf); cbor_put_bool(&w, 0); cbor_put_bool(&w, 1); cbor_put_null(&w); same(&w, "f4f5f6", "false true null");
    cbor_w_init(&w, buf, sizeof buf); cbor_put_tag(&w, 1); cbor_put_uint(&w, 1363896240); same(&w, "c11a514b67b0", "tag 1");

    /* ошибки писателя: нехватка места, плохой UTF-8; ошибка липкая */
    { uint8_t tiny[3]; cbor_w_init(&w, tiny, sizeof tiny); cbor_put_uint(&w, 1000000); CHECK(w.err == CBOR_ERR_SPACE && w.n == 0, "space"); cbor_put_uint(&w, 1); CHECK(w.err == CBOR_ERR_SPACE, "sticky error"); }
    cbor_w_init(&w, buf, sizeof buf); cbor_put_text(&w, "\xc0\x80", 2); CHECK(w.err == CBOR_ERR_FORMAT, "overlong utf8 rejected on write");
    cbor_w_init(&w, buf, sizeof buf); cbor_put_text(&w, "\xed\xa0\x80", 3); CHECK(w.err == CBOR_ERR_FORMAT, "surrogate rejected");
    cbor_w_init(&w, buf, sizeof buf); cbor_put_text(&w, "\xf4\x90\x80\x80", 4); CHECK(w.err == CBOR_ERR_FORMAT, "above U+10FFFF rejected");

    /* чтение: карта с вложенными значениями */
    { uint8_t m[64]; size_t n = unhex("a26161016162820203", m); cbor_item it; cbor_r at;
      CHECK(cbor_check(m, n, 1) == CBOR_OK, "check strict");
      CHECK(cbor_map_find(m, n, "a", &it, &at) == 0 && it.type == CBOR_UINT && it.val == 1, "find a");
      CHECK(cbor_map_find(m, n, "b", &it, &at) == 0 && it.type == CBOR_ARRAY && it.val == 2, "find b");
      CHECK(cbor_map_find(m, n, "c", &it, &at) == CBOR_NOT_FOUND, "missing key");
      CHECK(cbor_map_find(m, n, "", &it, &at) == CBOR_NOT_FOUND, "empty key missing");
      uint8_t arr[8]; size_t an = unhex("83010203", arr); CHECK(cbor_map_find(arr, an, "a", &it, &at) == CBOR_ERR_TYPE, "not a map"); }
    { uint8_t b[16]; cbor_item it; cbor_r r;
      size_t n = unhex("3903e7", b); cbor_r_init(&r, b, n, 1); CHECK(cbor_next(&r, &it) == 0 && it.type == CBOR_NINT && it.val == 999, "nint -1000 -> val 999");
      n = unhex("f93c00", b); cbor_r_init(&r, b, n, 1); CHECK(cbor_next(&r, &it) == 0 && it.type == CBOR_FLOAT && it.val == 0x3c00, "half float raw bits");
      n = unhex("fb3ff199999999999a", b); CHECK(cbor_check(b, n, 1) == CBOR_OK, "double skipped"); }

    /* строгий режим: не наименьшая длина, неопределённая длина; в нестрогом режиме число с лишними байтами допустимо */
    { uint8_t b[16]; size_t n = unhex("1800", b);   /* 0 как uint8 */
      CHECK(cbor_check(b, n, 1) == CBOR_ERR_STRICT, "strict: non-minimal int"); CHECK(cbor_check(b, n, 0) == CBOR_OK, "lenient: non-minimal int ok");
      n = unhex("5f4101ff", b); CHECK(cbor_check(b, n, 1) == CBOR_ERR_STRICT, "strict: indefinite length"); CHECK(cbor_check(b, n, 0) != CBOR_OK, "indefinite unsupported");
      n = unhex("1c", b); CHECK(cbor_check(b, n, 0) == CBOR_ERR_FORMAT, "reserved additional info"); }
    /* обрезанные и «враждебные» входы */
    { uint8_t b[16]; size_t n;
      n = unhex("1903", b); CHECK(cbor_check(b, n, 0) == CBOR_ERR_TRUNC, "truncated uint");
      n = unhex("6449", b); CHECK(cbor_check(b, n, 0) == CBOR_ERR_TRUNC, "truncated text");
      n = unhex("9bffffffffffffffff", b); CHECK(cbor_check(b, n, 0) == CBOR_ERR_TRUNC, "huge array count not allocated");
      n = unhex("0000", b); CHECK(cbor_check(b, n, 0) == CBOR_ERR_FORMAT, "trailing garbage");
      n = unhex("62c328", b); CHECK(cbor_check(b, n, 0) == CBOR_ERR_FORMAT, "bad utf8 on read");
      CHECK(cbor_check(b, 0, 0) == CBOR_ERR_TRUNC, "empty input"); }
    { uint8_t deep[40]; for (int i = 0; i < 40; i++) deep[i] = 0x81; deep[39] = 0x00;       /* вложенность 39 > предела */
      CHECK(cbor_check(deep, sizeof deep, 0) == CBOR_ERR_DEPTH, "depth limit"); }

    /* круговая проверка: запись -> чтение на случайных деревьях, и случайный мусор не роняет читатель */
    srand(12345);
    for (int round = 0; round < 2000; round++) {
        uint8_t g[128]; size_t n = (size_t)(rand() % 128);
        for (size_t i = 0; i < n; i++) g[i] = (uint8_t)rand();
        if (round % 2 && n) g[0] = (uint8_t)(0x80 | (rand() & 0x3f));            /* чаще массивы/карты/строки */
        (void)cbor_check(g, n, round & 2);
        cbor_item it; cbor_r at; (void)cbor_map_find(g, n, "k", &it, &at);
    }
    for (int round = 0; round < 500; round++) {
        cbor_w_init(&w, buf, sizeof buf);
        int cnt = rand() % 6; cbor_put_array(&w, (uint64_t)cnt);
        int64_t vals[6];
        for (int i = 0; i < cnt; i++) { vals[i] = (int64_t)(((uint64_t)rand() << 40) ^ ((uint64_t)rand() << 20) ^ (uint64_t)rand()) >> (rand() % 60); if (rand() & 1) vals[i] = -vals[i]; cbor_put_int(&w, vals[i]); }
        CHECK(w.err == 0 && cbor_check(buf, w.n, 1) == CBOR_OK, "roundtrip strict");
        cbor_r r; cbor_item it; cbor_r_init(&r, buf, w.n, 1); cbor_next(&r, &it);
        for (int i = 0; i < cnt; i++) {
            cbor_next(&r, &it);
            int64_t got = it.type == CBOR_UINT ? (int64_t)it.val : -1 - (int64_t)it.val;
            if (got != vals[i]) { CHECK(0, "roundtrip value"); break; }
        }
    }
    printf(fails ? "CBOR FAILED\n" : "cbor: RFC 8949 vectors, deterministic encoding, strict mode, bounds, fuzz passed\n");
    return fails != 0;
}
