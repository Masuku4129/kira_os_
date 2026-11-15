/* Сообщения портов (userland/kmsg.h): круговой обмен, детерминизм, строгость, отказ на мусоре и на каждой порче. */
#include "kmsg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

int main(void) {
    uint8_t b[64], c[64]; char op[8]; uint64_t n;
    uint32_t len = msg_make(b, sizeof b, "ping", 12345, 1);
    CHECK(len > 0 && cbor_check(b, len, 1) == CBOR_OK, "ping - строгий CBOR");
    CHECK(msg_parse(b, len, op, &n) == 0 && !strcmp(op, "ping") && n == 12345, "ping разобран");
    CHECK(msg_make(c, sizeof c, "ping", 12345, 1) == len && !memcmp(b, c, len), "детерминированность");
    len = msg_make(b, sizeof b, "err", 3, 1);
    CHECK(len > 0 && msg_parse(b, len, op, &n) == 0 && !strcmp(op, "err") && n == 0, "err разобран (code отдельным ключом)");
    cbor_item it; CHECK(cbor_map_find(b, len, "code", &it, 0) == CBOR_OK && it.type == CBOR_UINT && it.val == 3, "code найден");
    len = msg_make(b, sizeof b, "done", 0, 0);
    CHECK(msg_parse(b, len, op, &n) == 0 && !strcmp(op, "done"), "done без n");
    CHECK(msg_make(b, 3, "ping", 1, 1) == 0, "мало места - 0");
    /* каждое усечение верного сообщения отвергается */
    len = msg_make(b, sizeof b, "ping", 7, 1);
    int all = 1; for (uint32_t k = 0; k < len; k++) if (msg_parse(b, k, op, &n) == 0) all = 0;
    CHECK(all, "усечения отвергнуты");
    CHECK(msg_parse(b, len + 0, op, &n) == 0, "целое принято");
    memcpy(c, b, len); c[len] = 0; CHECK(msg_parse(c, len + 1, op, &n) == E_FORMAT, "хвост после сообщения отвергнут");
    /* нестрогое кодирование числа */
    static const uint8_t nonmin[] = { 0xA2, 0x61, 'n', 0x18, 0x05, 0x62, 'o', 'p', 0x64, 'p', 'i', 'n', 'g' };
    CHECK(msg_parse(nonmin, sizeof nonmin, op, &n) == E_FORMAT, "число не наименьшей длины отвергнуто");
    static const uint8_t noop[] = { 0xA1, 0x61, 'n', 0x01 };
    CHECK(msg_parse(noop, sizeof noop, op, &n) == E_NOOP, "нет op");
    static const uint8_t optype[] = { 0xA1, 0x62, 'o', 'p', 0x01 };
    CHECK(msg_parse(optype, sizeof optype, op, &n) == E_TYPE, "op не текст");
    static const uint8_t long_op[] = { 0xA1, 0x62, 'o', 'p', 0x68, 'a','b','c','d','e','f','g','h' };
    CHECK(msg_parse(long_op, sizeof long_op, op, &n) == E_TYPE, "слишком длинный op");
    static const uint8_t ntype[] = { 0xA2, 0x61, 'n', 0x61, 'x', 0x62, 'o', 'p', 0x64, 'p', 'i', 'n', 'g' };
    CHECK(msg_parse(ntype, sizeof ntype, op, &n) == E_TYPE, "n не число");
    static const uint8_t indef[] = { 0xBF, 0x62, 'o', 'p', 0x64, 'p', 'i', 'n', 'g', 0xFF };
    CHECK(msg_parse(indef, sizeof indef, op, &n) != 0, "неопределённая длина отвергнута");
    /* случайный мусор и мутации: без падений (ASan/UBSan), результат - 0 или код 1..3 */
    unsigned s = 12345; int bad_code = 0;
    for (int it2 = 0; it2 < 200000; it2++) {
        uint8_t g[48]; uint32_t gl = (s = s * 1103515245u + 12345u) >> 16 & 31;
        if (it2 & 1) { memcpy(g, b, len); gl = len; for (int m = 0; m < 3; m++) { s = s * 1103515245u + 12345u; g[(s >> 8) % len] ^= (uint8_t)(1u << ((s >> 4) & 7)); } }
        else for (uint32_t k = 0; k < gl; k++) { s = s * 1103515245u + 12345u; g[k] = (uint8_t)(s >> 16); }
        int r = msg_parse(g, gl, op, &n);
        if (r < 0 || r > E_TYPE) bad_code = 1;
        if (r == 0 && (op[0] == 0 || strlen(op) > 7)) bad_code = 1;
    }
    CHECK(!bad_code, "мусор: только допустимые коды");
    if (fails) { printf("kmsg: %d FAIL\n", fails); return 1; }
    printf("kmsg: CBOR port messages (round trip, deterministic, strict, truncation, junk and mutation fuzz) passed\n");
    return 0;
}
