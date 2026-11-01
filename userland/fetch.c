/* fetch адрес[:порт] [путь] - запрос HTTP/1.0 GET через сеть ядра (системные вызовы k_net_*), ответ сохраняется в
   песочницу (/data/fetch/last.txt, до 64 КБ), в терминал - строка состояния и итог. Пример: fetch example.com / */
#include "kira.h"
KIRA_ENTRY()

static char buf[65536];
static void say(const char* s) { k_write(s, k_strlen(s)); }
static void say_u(u64 v) { char t[21]; int i = 20; t[i] = 0; do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v); say(t + i); }

int umain(void) {
    char a[128];
    if (k_args(a, sizeof a) <= 0) { say("fetch: usage: fetch host[:port] [path]\n"); return 1; }
    char host[96]; u32 n = 0, port = 80;
    const char* p = a;
    while (*p && *p != ' ' && *p != ':' && *p != '/' && n + 1 < sizeof host) host[n++] = *p++;
    host[n] = 0;
    if (*p == ':') { p++; port = 0; while (*p >= '0' && *p <= '9') port = port * 10 + (u32)(*p++ - '0'); }
    while (*p == ' ') p++;
    const char* path = *p ? p : "/";
    i64 ip = k_net_dns(host);
    if (ip < 0) { say("fetch: address not found: "); say(host); say("\n"); return 1; }
    u64 t0 = k_clock_ns();
    i64 h = k_net_connect((u32)ip, port);
    if (h < 0) { say("fetch: connection refused or no network\n"); return 1; }
    char req[256]; u32 k = 0;
    const char* parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host, "\r\nUser-Agent: KiraOS-fetch\r\nConnection: close\r\n\r\n" };
    for (u32 i = 0; i < 5; i++) for (const char* c = parts[i]; *c && k + 1 < sizeof req; c++) req[k++] = *c;
    if (k_net_send(h, req, k) != (i64)k) { say("fetch: request not sent\n"); k_net_close(h); return 1; }
    u64 total = 0; i64 r;
    while ((r = k_net_recv(h, buf + (total < sizeof buf ? total : sizeof buf - 1024), 1024, 10000)) > 0) {
        if (total + (u64)r <= sizeof buf) total += (u64)r; else total = sizeof buf;
    }
    k_net_close(h);
    u64 ms = (k_clock_ns() - t0) / 1000000;
    u32 line = 0; while (line < total && line < 120 && buf[line] != '\r' && buf[line] != '\n') line++;
    say("fetch: "); say(host); say(path); say(" - ");
    k_write(buf, line);
    say(" - "); say_u(total); say(" bytes in "); say_u(ms); say(" ms");
    say(r == 0 ? " (server closed)" : " (connection ended with an error)");
    if (k_file_write("last.txt", buf, total) >= 0) say(", saved to /data/fetch/last.txt");
    say("\n");
    return r == 0 ? 0 : 1;
}
