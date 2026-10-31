/* Чистые функции сетевого стека: контрольная сумма (пример из RFC 1071 и заголовок IPv4), разбор адреса. */
#include <stdio.h>
#include <stdint.h>
#include "../net.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
int main(void) {
    uint8_t ip[20] = { 0x45,0x00,0x00,0x73,0x00,0x00,0x40,0x00,0x40,0x11,0x00,0x00,0xc0,0xa8,0x00,0x01,0xc0,0xa8,0x00,0xc7 };
    uint16_t c = net_csum(ip, 20);
    CHECK(c == 0xb861);                                  /* известный пример заголовка */
    ip[10] = (uint8_t)(c >> 8); ip[11] = (uint8_t)c;
    CHECK(net_csum(ip, 20) == 0);                        /* с суммой внутри - ноль */
    uint8_t odd[3] = { 1, 2, 3 };
    CHECK(net_csum(odd, 3) == (uint16_t)~(0x0102 + 0x0300));
    uint32_t a;
    CHECK(net_parse_ip("10.0.2.2", &a) == 0 && a == 0x0A000202);
    CHECK(net_parse_ip("255.255.255.255 4", &a) == 0 && a == 0xFFFFFFFF);
    CHECK(net_parse_ip("256.1.1.1", &a) != 0);
    CHECK(net_parse_ip("1.2.3", &a) != 0);
    CHECK(net_parse_ip("1.2.3.4x", &a) != 0);
    CHECK(net_parse_ip("", &a) != 0);
    if (fails) return 1;
    printf("net: checksum, address parsing passed\n");
    return 0;
}
