/* net6 адрес_IPv6 порт_UDP порт_TCP - проверка IPv6 для программ: датаграмма UDP на адрес и ответ собеседника, затем
   соединение TCP с запросом GET (HTTP/1.0). Сокеты семейства 6 (вызовы 44-47, userland/kira.h). Адрес разбирает ip6.c. */
#include "kira.h"
#include "ip6.h"
KIRA_ENTRY()

static void say(const char* s) { k_write(s, k_strlen(s)); }
static void say_u(u64 v) { char t[21]; int i = 20; t[i] = 0; do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v); say(t + i); }
static u32 num(const char** p) { u32 v = 0; while (**p == ' ') (*p)++; while (**p >= '0' && **p <= '9') { v = v * 10 + (u32)(*(*p)++ - '0'); } return v; }

int umain(void) {
    char a[96]; const char* p = a;
    if (k_args(a, sizeof a) <= 0) a[0] = 0;
    ksockaddr_t to;
    for (u32 i = 0; i < sizeof to; i++) ((u8*)&to)[i] = 0;
    to.family = 6;
    if (ip6_parse(a, to.addr) != 0) { say("net6: usage: net6 <IPv6 address> <UDP port> <TCP port>\n"); return 1; }
    while (*p && *p != ' ') p++;
    u32 uport = num(&p), tport = num(&p);

    i64 s = k_udp_open_fam(0, 6);
    if (s < 0) { say("net6: no IPv6 on this machine (UDP socket refused)\n"); return 1; }
    to.port = (u16)uport;
    static const char msg[] = "hello6";
    if (k_udp_sendto(s, &to, msg, sizeof msg - 1) != (i64)(sizeof msg - 1)) { say("net6: udp send failed\n"); return 1; }
    static u8 buf[24 + 1500];
    i64 n = k_udp_recvfrom(s, buf, sizeof buf, 5000);
    if (n < 0) { say("net6: udp: no reply\n"); return 1; }
    ksockaddr_t* from = (ksockaddr_t*)buf;
    char ra[40];
    ip6_format(from->addr, ra);
    say("net6: udp reply '"); k_write((const char*)buf + 24, (u64)n); say("' from ["); say(ra); say("]:"); say_u(from->port);
    say(" family "); say_u(from->family); say("\n");
    k_udp_close(s);

    to.port = (u16)tport;
    i64 h = k_net_connect_sa(&to);
    if (h < 0) { say("net6: tcp connect failed, code "); say_u((u64)(-h)); say("\n"); return 1; }
    ksockaddr_t peer;
    i64 fam = k_net_peer(h, &peer);
    static const char req[] = "GET /net6 HTTP/1.0\r\nHost: kira\r\nConnection: close\r\n\r\n";
    if (k_net_send(h, req, sizeof req - 1) != (i64)(sizeof req - 1)) { say("net6: tcp send failed\n"); k_net_close(h); return 1; }
    static u8 body[2048];
    u32 total = 0, first = 0;
    for (;;) {
        i64 r = k_net_recv(h, body + (total < sizeof body ? total : 0), total < sizeof body ? (u32)(sizeof body - total) : 64, 5000);
        if (r <= 0) break;
        total += (u32)r;
    }
    k_net_close(h);
    if (total > sizeof body) total = sizeof body;
    while (first < total && body[first] != '\r' && body[first] != '\n') first++;
    say("net6: tcp peer family "); say_u((u64)fam); say(", "); say_u(total); say(" bytes, first line '");
    k_write((const char*)body, first); say("'\n");
    return 0;
}
