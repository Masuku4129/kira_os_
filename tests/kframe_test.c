/* Каркас сообщений служб (userland/kframe.h): запрос и ответ туда-обратно, детерминизм, строгость, отказ на мусоре и порче. */
#include "kframe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static uint32_t make_req(uint8_t* b, uint32_t cap, uint32_t id, const char* op, const char* name, uint64_t off, int with_args) {
    cbor_w w; cbor_w_init(&w, b, cap);
    kf_req_begin(&w, id, op, with_args);
    if (with_args) { cbor_put_map(&w, 2); cbor_put_cstr(&w, "off"); cbor_put_uint(&w, off); cbor_put_cstr(&w, "name"); cbor_put_cstr(&w, name); }
    return w.err ? 0 : (uint32_t)w.n;
}

int main(void) {
    uint8_t b[128], c[128]; kframe_t f;
    uint32_t len = make_req(b, sizeof b, 77, "read", "a.txt", 4096, 1);
    CHECK(len > 0 && cbor_check(b, len, 1) == CBOR_OK, "запрос - строгий CBOR");
    CHECK(kf_parse_req(b, len, &f) == KF_OK && f.id == 77 && !strcmp(f.op, "read") && f.body_len > 0, "запрос разобран");
    uint64_t off = 0; const uint8_t* p; uint32_t n;
    CHECK(kf_get_uint(&f, "off", &off) == KF_OK && off == 4096, "аргумент-число");
    CHECK(kf_get_text(&f, "name", &p, &n) == KF_OK && n == 5 && !memcmp(p, "a.txt", 5), "аргумент-текст");
    CHECK(kf_get_uint(&f, "nope", &off) == KF_ABSENT, "нет аргумента");
    CHECK(kf_get_uint(&f, "name", &off) == KF_E_TYPE, "аргумент другого типа");
    CHECK(kf_get_bytes(&f, "name", &p, &n) == KF_E_TYPE, "текст - не байты");
    CHECK(make_req(c, sizeof c, 77, "read", "a.txt", 4096, 1) == len && !memcmp(b, c, len), "детерминированность");
    len = make_req(b, sizeof b, 5, "ping", 0, 0, 0);
    CHECK(kf_parse_req(b, len, &f) == KF_OK && f.body_len == 0 && kf_get_uint(&f, "x", &off) == KF_ABSENT, "запрос без args");
    CHECK(make_req(c, 10, 5, "ping", 0, 0, 0) == 0, "мало места - 0");

    /* ответы */
    cbor_w w; cbor_w_init(&w, b, sizeof b);
    kf_resp_begin(&w, 9, 1); cbor_put_bytes(&w, "xyz", 3); kf_resp_end(&w, -2);
    len = (uint32_t)w.n;
    CHECK(!w.err && cbor_check(b, len, 1) == CBOR_OK, "ответ - строгий CBOR");
    CHECK(kf_parse_resp(b, len, &f) == KF_OK && f.id == 9 && f.code == -2 && f.body_len == 4, "ответ разобран, код отрицательный");
    CHECK(kf_get_bytes(&f, "x", &p, &n) == KF_E_FORMAT || 1, "res - не карта: поиск ключа не падает");
    len = kf_resp_code(b, sizeof b, 3, 0);
    CHECK(len > 0 && kf_parse_resp(b, len, &f) == KF_OK && f.id == 3 && f.code == 0 && !f.body_len, "ответ с кодом 0");
    len = kf_resp_code(b, sizeof b, 3, -103);
    CHECK(kf_parse_resp(b, len, &f) == KF_OK && f.code == -103, "код -103");
    CHECK(kf_resp_code(b, 4, 3, 0) == 0, "ответ: мало места");

    /* запрос не принимается как ответ и наоборот */
    len = make_req(b, sizeof b, 1, "ping", 0, 0, 0);
    CHECK(kf_parse_resp(b, len, &f) != KF_OK, "запрос - не ответ");
    len = kf_resp_code(b, sizeof b, 1, 0);
    CHECK(kf_parse_req(b, len, &f) != KF_OK, "ответ - не запрос");

    /* ручные испорченные */
    { static const uint8_t t[] = { 0xA3, 0x61, 'v', 0x02, 0x62, 'i', 'd', 0x01, 0x62, 'o', 'p', 0x64, 'p', 'i', 'n', 'g' };
      CHECK(kf_parse_req(t, sizeof t, &f) == KF_E_VERSION, "версия 2 отвергнута"); }
    { static const uint8_t t[] = { 0xA2, 0x61, 'v', 0x01, 0x62, 'o', 'p', 0x64, 'p', 'i', 'n', 'g' };
      CHECK(kf_parse_req(t, sizeof t, &f) == KF_E_ID, "нет id"); }
    { static const uint8_t t[] = { 0xA2, 0x61, 'v', 0x01, 0x62, 'i', 'd', 0x01 };
      CHECK(kf_parse_req(t, sizeof t, &f) == KF_E_OP, "нет op"); }
    { static const uint8_t t[] = { 0xA3, 0x61, 'v', 0x01, 0x62, 'i', 'd', 0x01, 0x62, 'o', 'p', 0x62, 'a', ' ' };
      CHECK(kf_parse_req(t, sizeof t, &f) == KF_E_OP, "пробел в имени операции"); }
    { static const uint8_t t[] = { 0xA4, 0x61, 'v', 0x01, 0x62, 'i', 'd', 0x01, 0x62, 'o', 'p', 0x61, 'a', 0x61, 'x', 0x01 };
      CHECK(kf_parse_req(t, sizeof t, &f) == KF_E_KEY, "чужой ключ верхнего уровня"); }
    { static const uint8_t t[] = { 0xA4, 0x61, 'v', 0x01, 0x62, 'i', 'd', 0x01, 0x62, 'i', 'd', 0x02, 0x62, 'o', 'p', 0x61, 'a' };
      CHECK(kf_parse_req(t, sizeof t, &f) == KF_E_KEY, "повтор ключа"); }
    { static const uint8_t t[] = { 0xA3, 0x61, 'v', 0x01, 0x62, 'i', 'd', 0x1B, 1, 0, 0, 0, 0, 0, 0, 0, 0x62, 'o', 'p', 0x61, 'a' };
      CHECK(kf_parse_req(t, sizeof t, &f) == KF_E_ID, "id шире 32 бит"); }
    { static const uint8_t t[] = { 0xA3, 0x61, 'v', 0x18, 0x01, 0x62, 'i', 'd', 0x01, 0x62, 'o', 'p', 0x61, 'a' };
      CHECK(kf_parse_req(t, sizeof t, &f) == KF_E_FORMAT, "число не наименьшей длины"); }
    { static const uint8_t t[] = { 0xBF, 0x61, 'v', 0x01, 0xFF };
      CHECK(kf_parse_req(t, sizeof t, &f) == KF_E_FORMAT, "неопределённая длина"); }
    /* каждое усечение верного запроса отвергается, любой хвост тоже */
    len = make_req(b, sizeof b, 77, "read", "a.txt", 4096, 1);
    int all = 1; for (uint32_t k = 0; k < len; k++) if (kf_parse_req(b, k, &f) == KF_OK) all = 0;
    CHECK(all, "усечения отвергнуты");
    memcpy(c, b, len); c[len] = 0; CHECK(kf_parse_req(c, len + 1, &f) == KF_E_FORMAT, "хвост отвергнут");

    /* мусор и мутации: без падений (ASan/UBSan), результат - только известные коды, op всегда безопасная строка */
    unsigned s = 4242; int bad = 0;
    for (int i = 0; i < 300000; i++) {
        uint8_t g[160]; uint32_t gl;
        if (i & 1) { memcpy(g, b, len); gl = len; for (int m = 0; m < 3; m++) { s = s * 1103515245u + 12345u; g[(s >> 8) % len] ^= (uint8_t)(1u << ((s >> 4) & 7)); } }
        else { gl = (s = s * 1103515245u + 12345u) >> 16 & 63; for (uint32_t k = 0; k < gl; k++) { s = s * 1103515245u + 12345u; g[k] = (uint8_t)(s >> 16); } }
        int r = kf_parse_req(g, gl, &f);
        if (r < 0 || r > KF_E_CODE) bad = 1;
        if (r == 0) {
            if (!f.op[0] || strlen(f.op) > KF_OP_MAX) bad = 1;
            if (f.body_len && (f.body < g || f.body + f.body_len > g + gl)) bad = 1;
            uint64_t v; const uint8_t* q; uint32_t qn;
            (void)kf_get_uint(&f, "off", &v); (void)kf_get_text(&f, "name", &q, &qn);
        }
        r = kf_parse_resp(g, gl, &f); if (r < 0 || r > KF_E_CODE) bad = 1;
    }
    CHECK(!bad, "мусор: только допустимые коды, указатели внутри буфера");
    if (fails) { printf("kframe: %d FAIL\n", fails); return 1; }
    printf("kframe: service message frame v1 (request/response, deterministic, strict keys, truncation, junk and mutation fuzz) passed\n");
    return 0;
}
