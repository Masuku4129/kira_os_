/* Хост-тест чистой части удалённой консоли: ключ, проверка пароля, разбор AUTH. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../rcon.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    uint8_t salt[16], k[RCON_KEY_LEN], k2[RCON_KEY_LEN];
    for (int i = 0; i < 16; i++) salt[i] = (uint8_t)(i * 7 + 1);
    rcon_make_key(salt, "correct horse", k);
    CHECK(memcmp(k, salt, 16) == 0);
    CHECK(rcon_check_pass(k, "correct horse"));
    CHECK(!rcon_check_pass(k, "correct horsf"));
    CHECK(!rcon_check_pass(k, ""));
    CHECK(!rcon_check_pass(k, "correct horse "));
    salt[0] ^= 1;
    rcon_make_key(salt, "correct horse", k2);
    CHECK(memcmp(k + 16, k2 + 16, 32) != 0);            /* другая соль - другой ключ */
    char l1[] = "AUTH secret123\r\n", l2[] = "AUTH ", l3[] = "auth x", l4[] = "AUTHsecret", l5[] = "AUTH a b c\n";
    CHECK(rcon_parse_auth(l1) && strcmp(rcon_parse_auth(l1), "secret123") == 0);
    CHECK(rcon_parse_auth(l2) == 0);
    CHECK(rcon_parse_auth(l3) == 0);
    CHECK(rcon_parse_auth(l4) == 0);
    CHECK(rcon_parse_auth(l5) && strcmp(rcon_parse_auth(l5), "a b c") == 0);   /* пробелы в пароле допустимы */
    printf(fails ? "rcon_test: FAILED\n" : "rcon_test: OK\n");
    return fails != 0;
}
