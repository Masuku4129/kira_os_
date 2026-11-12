/* IPv6 на хосте: адреса (RFC 4291, 5952), EUI-64 и SLAAC, контрольные суммы с псевдозаголовком, заголовки расширения,
   сообщения ND (RS/RA/NS/NA) и эхо ICMPv6.
   Откуда векторы: пример суммы RFC 1071 п. 3; EUI-64 - RFC 4291, приложение A; solicited-node - RFC 4291 п. 2.7.1;
   текстовые формы - RFC 4291 п. 2.2 и RFC 5952 п. 4. В самих RFC числовых примеров суммы с псевдозаголовком IPv6 нет,
   поэтому векторы суммы получены отдельной реализацией на Python (struct + определение RFC 8200 п. 8.1 по полям,
   код не связан с ip6.c) - см. комментарии у массивов. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../ip6.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int hexb(const char* h, uint8_t* o) {
    int n = 0;
    for (; h[0] && h[1]; h += 2, n++) { unsigned v; sscanf(h, "%2x", &v); o[n] = (uint8_t)v; }
    return n;
}
static void addr(const char* s, uint8_t a[16]) { if (ip6_parse(s, a)) { printf("bad literal %s\n", s); fails++; } }
static int fmt_is(const char* in, const char* want) {
    uint8_t a[16]; char out[40];
    if (ip6_parse(in, a) != 0) return 0;
    ip6_format(a, out);
    if (strcmp(out, want)) printf("  format(%s) = %s, want %s\n", in, out, want);
    return !strcmp(out, want);
}

static void test_text(void) {
    /* RFC 4291 п. 2.2 */
    CHECK(fmt_is("ABCD:EF01:2345:6789:ABCD:EF01:2345:6789", "abcd:ef01:2345:6789:abcd:ef01:2345:6789"));
    CHECK(fmt_is("2001:DB8:0:0:8:800:200C:417A", "2001:db8::8:800:200c:417a"));
    CHECK(fmt_is("FF01:0:0:0:0:0:0:101", "ff01::101"));
    CHECK(fmt_is("0:0:0:0:0:0:0:1", "::1"));
    CHECK(fmt_is("0:0:0:0:0:0:0:0", "::"));
    CHECK(fmt_is("2001:DB8::8:800:200C:417A", "2001:db8::8:800:200c:417a"));
    CHECK(fmt_is("FF01::101", "ff01::101"));
    CHECK(fmt_is("::1", "::1"));
    CHECK(fmt_is("::", "::"));
    CHECK(fmt_is("0:0:0:0:0:0:13.1.68.3", "::d01:4403"));
    CHECK(fmt_is("::FFFF:129.144.52.38", "::ffff:8190:3426"));
    /* RFC 5952 п. 4: нули не сокращаются в одной группе, побеждает самая длинная, при равных - первая, без лишних нулей */
    CHECK(fmt_is("2001:db8:0:1:1:1:1:1", "2001:db8:0:1:1:1:1:1"));
    CHECK(fmt_is("2001:0:0:1:0:0:0:1", "2001:0:0:1::1"));
    CHECK(fmt_is("2001:db8:0:0:1:0:0:1", "2001:db8::1:0:0:1"));
    CHECK(fmt_is("2001:0db8:0000:0000:0000:0000:0000:0001", "2001:db8::1"));
    CHECK(fmt_is("fe80::5054:ff:fe12:3456", "fe80::5054:ff:fe12:3456"));
    CHECK(fmt_is("1::", "1::"));
    CHECK(fmt_is("0:0:1:0:0:0:0:0", "0:0:1::"));
    CHECK(fmt_is("fec0::2 хвост", "fec0::2"));                          /* разбор останавливается на пробеле */
    /* негодные */
    uint8_t a[16];
    CHECK(ip6_parse(0, a) == -1 && ip6_parse("::", 0) == -1);
    const char* bad[] = { "", ":", ":1", "1:", "1::2::3", "12345::", "1:2:3:4:5:6:7", "1:2:3:4:5:6:7:8:9", "g::", "::1::", "1:::2",
                          "1.2.3.4", "::1.2.3", "::1.2.3.256", "1:2:3:4:5:6:7::8", "::1x", "1:2:3:4:5:6:1.2.3.4:5" };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++)
        if (ip6_parse(bad[i], a) == 0) { printf("  accepted bad literal '%s'\n", bad[i]); fails++; }
    CHECK(ip6_parse("1:2:3:4:5:6:7::", a) == 0);                        /* «::» вместо одной группы разрешён при разборе */
    CHECK(ip6_parse("::2:3:4:5:6:7:8", a) == 0);
}

static void test_addr(void) {
    uint8_t a[16], b[16], mac[6] = { 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE }, iid[8], want[16];
    /* RFC 4291, приложение A: 34-56-78-9A-BC-DE -> 3656:78FF:FE9A:BCDE */
    ip6_eui64(mac, iid);
    const uint8_t e[8] = { 0x36, 0x56, 0x78, 0xFF, 0xFE, 0x9A, 0xBC, 0xDE };
    CHECK(!memcmp(iid, e, 8));
    ip6_linklocal(mac, a);
    addr("fe80::3656:78ff:fe9a:bcde", want);
    CHECK(!memcmp(a, want, 16));
    CHECK(ip6_is_linklocal(a) && !ip6_is_mcast(a) && !ip6_is_unspec(a));
    /* QEMU: 52:54:00:12:34:56 */
    uint8_t q[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    ip6_linklocal(q, a);
    addr("fe80::5054:ff:fe12:3456", want);
    CHECK(!memcmp(a, want, 16));
    /* SLAAC: /64 */
    addr("fec0::", b);
    CHECK(ip6_slaac(b, 64, q, a) == 0);
    addr("fec0::5054:ff:fe12:3456", want);
    CHECK(!memcmp(a, want, 16));
    CHECK(ip6_slaac(b, 48, q, a) == -1);
    CHECK(ip6_slaac(b, 128, q, a) == -1);
    /* RFC 4291 п. 2.7.1 */
    addr("4037::01:800:200E:8C6C", a);
    ip6_solicited(a, b);
    addr("ff02::1:ff0e:8c6c", want);
    CHECK(!memcmp(b, want, 16));
    CHECK(ip6_is_mcast(b));
    uint8_t m[6];
    ip6_mcast_mac(b, m);                                                  /* RFC 2464 п. 7: 33:33 + последние 32 бита */
    const uint8_t wm[6] = { 0x33, 0x33, 0xFF, 0x0E, 0x8C, 0x6C };
    CHECK(!memcmp(m, wm, 6));
    /* префиксы */
    addr("2001:db8:1234:5678::1", a); addr("2001:db8:1234:5679::1", b);
    CHECK(ip6_prefix_eq(a, b, 64) == 0);
    CHECK(ip6_prefix_eq(a, b, 63) == 1);
    CHECK(ip6_prefix_eq(a, b, 0) == 1);
    CHECK(ip6_prefix_eq(a, a, 128) == 1);
    CHECK(ip6_prefix_eq(a, b, 200) == 0);
}

static void test_sum(void) {
    /* RFC 1071 п. 3: слова 0001 f203 f4f5 f6f7, сумма ddf2, дополнение 220d */
    const uint8_t r[8] = { 0x00, 0x01, 0xF2, 0x03, 0xF4, 0xF5, 0xF6, 0xF7 };
    uint32_t s = net_sum(0, r, 8);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    CHECK(s == 0xDDF2);
    CHECK(net_sum_end(net_sum(0, r, 8)) == 0x220D);
    /* по частям (чётные границы) - то же */
    CHECK(net_sum_end(net_sum(net_sum(net_sum(0, r, 2), r + 2, 4), r + 6, 2)) == 0x220D);
    /* нечётный хвост */
    const uint8_t o[3] = { 1, 2, 3 };
    CHECK(net_sum_end(net_sum(0, o, 3)) == (uint16_t)~(0x0102 + 0x0300));
    /* большой вход не переполняет сумму */
    static uint8_t big[70000];
    memset(big, 0xFF, sizeof big);
    CHECK(net_sum_end(net_sum(0, big, sizeof big)) == 0);

    /* IPv4 (Python): TCP SYN 10.0.2.15 -> 10.0.2.100, UDP "hello" */
    uint8_t t[64];
    int n = hexb("c000005001020304000000006002ffff00000000020405b4", t);
    CHECK(net_csum4(0x0A00020F, 0x0A000264, 6, t, (uint32_t)n) == 47965);
    n = hexb("13882328000d000068656c6c6f", t);
    CHECK(net_csum4(0x0A00020F, 0x0A000264, 17, t, (uint32_t)n) == 27871);

    /* IPv6 (Python, RFC 8200 п. 8.1). Сумма в поле нулевая; с правильной суммой внутри пакет даёт 0. */
    static const struct { const char* name; const char* s; const char* d; uint8_t nh; const char* hex; uint16_t want; } v[] = {
        { "echo",    "2001:db8::1", "2001:db8::2", 58, "800000001234000161626364", 19784 },
        { "udp",     "fe80::5054:ff:fe12:3456", "fec0::2", 17, "0fa02328000e000068656c6c6f36", 2050 },
        { "tcp",     "fec0::15", "fec0::2", 6, "c000005001020304000000006002ffff00000000020405a0", 54859 },
        { "ns",      "fe80::5054:ff:fe12:3456", "ff02::1:ff00:2", 58, "8700000000000000fec000000000000000000000000000020101525400123456", 29152 },
        { "na",      "fec0::2", "fe80::5054:ff:fe12:3456", 58, "8800000060000000fec00000000000000000000000000002020152550a000202", 14217 },
        { "rs",      "fe80::5054:ff:fe12:3456", "ff02::2", 58, "85000000000000000101525400123456", 29109 },
        { "udp_odd", "2001:db8::1", "2001:db8::2", 17, "00010002000b00006f6464", 53499 },
    };
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
        uint8_t sa[16], da[16], p[128];
        addr(v[i].s, sa); addr(v[i].d, da);
        int len = hexb(v[i].hex, p);
        uint16_t c = net_csum6(sa, da, v[i].nh, p, (uint32_t)len);
        if (c != v[i].want) { printf("  %s: sum %u, want %u\n", v[i].name, c, v[i].want); fails++; }
        int off = v[i].nh == 58 ? 2 : v[i].nh == 17 ? 6 : 16;
        p[off] = (uint8_t)(v[i].want >> 8); p[off + 1] = (uint8_t)v[i].want;
        CHECK(net_csum6(sa, da, v[i].nh, p, (uint32_t)len) == 0);       /* с суммой внутри */
        p[len - 1] ^= 1;                                                  /* порча одного бита ловится */
        CHECK(net_csum6(sa, da, v[i].nh, p, (uint32_t)len) != 0);
    }
    /* адрес псевдозаголовка входит в сумму */
    uint8_t sa[16], da[16], da2[16], p[16];
    addr("2001:db8::1", sa); addr("2001:db8::2", da); addr("2001:db8::3", da2);
    int len = hexb("800000001234000161626364", p);
    CHECK(net_csum6(sa, da, 58, p, (uint32_t)len) != net_csum6(sa, da2, 58, p, (uint32_t)len));
    CHECK(net_csum6(sa, da, 58, p, (uint32_t)len) != net_csum6(sa, da, 17, p, (uint32_t)len));
}

static void test_build(void) {
    /* строители дают ровно те же байты, что отдельная реализация (включая сумму) */
    uint8_t o[128], want[128], q[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 }, sa[16], da[16], tg[16];
    addr("fe80::5054:ff:fe12:3456", sa); addr("ff02::1:ff00:2", da); addr("fec0::2", tg);
    uint32_t n = nd_build_ns(o, sa, da, tg, q);
    int wn = hexb("870071e0" "00000000" "fec00000000000000000000000000002" "0101525400123456", want);   /* сумма 29152 = 0x71e0 */
    CHECK((int)n == wn && !memcmp(o, want, n));
    /* NS для проверки адреса (DAD): источник «::», без опции */
    uint8_t un[16] = { 0 };
    n = nd_build_ns(o, un, da, tg, 0);
    CHECK(n == 24 && o[0] == 135 && net_csum6(un, da, 58, o, n) == 0);
    /* NA: флаги S и O, опция получателя */
    addr("fe80::5054:ff:fe12:3456", da); addr("fec0::2", sa);
    const uint8_t ml[6] = { 0x52, 0x55, 0x0A, 0x00, 0x02, 0x02 };
    n = nd_build_na(o, sa, da, tg, ND_NA_S | ND_NA_O, ml);
    wn = hexb("8800" "3789" "60000000" "fec00000000000000000000000000002" "020152550a000202", want);
    CHECK((int)n == wn && !memcmp(o, want, n));                          /* 0x3789 = 14217 */
    /* RS */
    addr("ff02::2", da); addr("fe80::5054:ff:fe12:3456", sa);
    n = nd_build_rs(o, sa, da, q);
    wn = hexb("85" "00" "71b5" "00000000" "0101525400123456", want);
    CHECK((int)n == wn && !memcmp(o, want, n));                          /* 0x71b5 = 29109 */
    /* эхо */
    addr("2001:db8::1", sa); addr("2001:db8::2", da);
    n = icmp6_build_echo(o, sa, da, 128, 0x1234, 1, (const uint8_t*)"abcd", 4);
    wn = hexb("80004d4812340001" "61626364", want);                       /* 19784 = 0x4d48 */
    CHECK((int)n == wn && !memcmp(o, want, n));
}

static void test_walk(void) {
    uint8_t b[64], proto = 0; uint32_t off = 0;
    CHECK(ip6_walk(0, 0, 17, &proto, &off) == 0);
    CHECK(ip6_walk(b, 0, 60, 0, &off) == -1);
    CHECK(ip6_walk(b, 0, 60, &proto, 0) == -1);
    /* без расширений */
    CHECK(ip6_walk(b, 20, 17, &proto, &off) == 0 && proto == 17 && off == 0);
    /* параметры получателя (8 байт) затем UDP */
    memset(b, 0, sizeof b); b[0] = 17; b[1] = 0;
    CHECK(ip6_walk(b, 40, 60, &proto, &off) == 0 && proto == 17 && off == 8);
    /* hop-by-hop на 16 байт, затем получателя, затем TCP */
    memset(b, 0, sizeof b); b[0] = 60; b[1] = 1; b[16] = 6; b[17] = 0;
    CHECK(ip6_walk(b, 40, 0, &proto, &off) == 0 && proto == 6 && off == 24);
    /* маршрутизация: остаток узлов 0 - проходим, не 0 - отказ (пересылать не умеем) */
    memset(b, 0, sizeof b); b[0] = 17; b[1] = 0; b[2] = 2; b[3] = 0;
    CHECK(ip6_walk(b, 32, 43, &proto, &off) == 0 && off == 8);
    b[3] = 1;
    CHECK(ip6_walk(b, 32, 43, &proto, &off) == -1);
    /* фрагмент: найден заголовок */
    memset(b, 0, sizeof b); b[0] = 17;
    CHECK(ip6_walk(b, 32, 44, &proto, &off) == 1 && off == 0);
    memset(b, 0, sizeof b); b[0] = 44; b[1] = 0; b[8] = 17;
    CHECK(ip6_walk(b, 32, 60, &proto, &off) == 1 && off == 8);
    /* усечённый и без места */
    CHECK(ip6_walk(b, 4, 60, &proto, &off) == -1);
    memset(b, 0, sizeof b); b[0] = 17; b[1] = 3;                        /* длина 32 байта, а данных 16 */
    CHECK(ip6_walk(b, 16, 60, &proto, &off) == -1);
    CHECK(ip6_walk(b, 16, 51, &proto, &off) == -1);                     /* AH не поддерживается */
    /* цепочка без конца ограничена */
    memset(b, 0, sizeof b);
    uint8_t chain[80]; memset(chain, 0, sizeof chain);
    for (int i = 0; i < 9; i++) { chain[8 * i] = 60; chain[8 * i + 1] = 0; }
    CHECK(ip6_walk(chain, 72, 60, &proto, &off) == -1);
}

static uint32_t put_pi(uint8_t* o, const char* prefix, uint8_t plen, uint8_t fl, uint32_t valid, uint32_t pref) {
    memset(o, 0, 32); o[0] = 3; o[1] = 4; o[2] = plen; o[3] = fl;
    o[4] = (uint8_t)(valid >> 24); o[5] = (uint8_t)(valid >> 16); o[6] = (uint8_t)(valid >> 8); o[7] = (uint8_t)valid;
    o[8] = (uint8_t)(pref >> 24); o[9] = (uint8_t)(pref >> 16); o[10] = (uint8_t)(pref >> 8); o[11] = (uint8_t)pref;
    addr(prefix, o + 16);
    return 32;
}

static void test_nd(void) {
    uint8_t m[512]; ra_t r;
    CHECK(nd_parse_ra(0, 16, &r) == -1);
    CHECK(nd_parse_ra(m, 16, 0) == -1);
    CHECK(nd_lladdr(0, 0, 1, (uint8_t[6]){0}) == 0);
    CHECK(nd_lladdr(0, 0, 1, 0) == -1);
    /* RA: hop 64, флаги O, срок 1800, SLLA, MTU 1500, префикс fec0::/64 (L+A, 86400/14400), RDNSS fec0::3 */
    memset(m, 0, sizeof m);
    m[0] = 134; m[4] = 64; m[5] = 0x40; m[6] = 0x07; m[7] = 0x08;
    uint32_t n = 16;
    m[n] = 1; m[n + 1] = 1; hexb("525500000002", m + n + 2); n += 8;
    m[n] = 5; m[n + 1] = 1; m[n + 4] = 0; m[n + 5] = 0; m[n + 6] = 0x05; m[n + 7] = 0xDC; n += 8;
    n += put_pi(m + n, "fec0::", 64, RA_PI_ONLINK | RA_PI_AUTO, 86400, 14400);
    m[n] = 25; m[n + 1] = 3; m[n + 4] = 0; m[n + 5] = 0; m[n + 6] = 0x0E; m[n + 7] = 0x10; addr("fec0::3", m + n + 8); n += 24;
    CHECK(nd_parse_ra(m, n, &r) == 0);
    CHECK(r.hop == 64 && r.flags == 0x40 && r.lifetime == 1800);
    CHECK(r.have_slla && r.slla[0] == 0x52 && r.slla[5] == 0x02);
    CHECK(r.mtu == 1500);
    CHECK(r.npi == 1 && r.pi[0].plen == 64 && r.pi[0].flags == 0xC0 && r.pi[0].valid == 86400 && r.pi[0].pref == 14400);
    uint8_t p[16]; addr("fec0::", p);
    CHECK(!memcmp(r.pi[0].prefix, p, 16));
    addr("fec0::3", p);
    CHECK(r.have_dns && !memcmp(r.dns, p, 16) && r.dns_life == 3600);
    /* лишние префиксы пропускаются, не ломая разбор */
    uint32_t k = n;
    for (int i = 0; i < 6; i++) k += put_pi(m + k, "2001:db8::", 64, RA_PI_AUTO, 100, 50);
    CHECK(nd_parse_ra(m, k, &r) == 0 && r.npi == RA_PI_MAX);
    /* испорчено: опция нулевой длины, опция за концом, мало данных, не RA */
    uint8_t bad[32]; memset(bad, 0, sizeof bad); bad[0] = 134; bad[16] = 1; bad[17] = 0;
    CHECK(nd_parse_ra(bad, 24, &r) == -1);
    memcpy(bad, m, 16); bad[16] = 3; bad[17] = 4;
    CHECK(nd_parse_ra(bad, 24, &r) == -1);
    CHECK(nd_parse_ra(m, 15, &r) == -1);
    m[0] = 135;
    CHECK(nd_parse_ra(m, n, &r) == -1);
    /* опции адреса канального уровня */
    uint8_t o[24], mac[6]; memset(o, 0, sizeof o);
    o[0] = 2; o[1] = 1; hexb("525500000002", o + 2);
    CHECK(nd_lladdr(o, 8, 2, mac) == 1 && mac[1] == 0x55);
    CHECK(nd_lladdr(o, 8, 1, mac) == 0);
    o[8] = 1; o[9] = 0;
    CHECK(nd_lladdr(o, 16, 2, mac) == -1);                              /* нулевая длина губит всё сообщение */
    o[9] = 2;
    CHECK(nd_lladdr(o, 16, 2, mac) == -1);                              /* длина за концом */
}

int main(void) {
    test_text(); test_addr(); test_sum(); test_build(); test_walk(); test_nd();
    if (fails) { printf("ip6: %d failures\n", fails); return 1; }
    printf("ip6: text forms, EUI-64/SLAAC, checksums (RFC 1071 + pseudo-header vectors), ND builders/parsers, extension headers passed\n");
    return 0;
}
