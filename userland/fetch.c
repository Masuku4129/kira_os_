/* fetch [-r] [https://]адрес[:порт] [путь] - запрос HTTP/1.0 GET через сеть ядра (системные вызовы k_net_*); с https:// -
   поверх TLS 1.3 (userland/tls.c: шифрование в самой программе). Ответ сохраняется в песочницу (/data/fetch/last.txt,
   до 64 КБ), в терминал - строка состояния и итог. Примеры: fetch example.com /, fetch https://example.com/
   Для https печатается ещё состояние OCSP (прикреплённый ответ: проверен / нет / без гарантии; отозван - отказ).
   -r: после первого запроса второе соединение идёт по билету сессии (PSK) - без сертификатов; печатается, принял ли сервер. */
#include "kira.h"
#include "tls.h"
KIRA_ENTRY()

static tls_t tls;
static int use_tls;
static u8 roots[512 * 1024];                     /* /etc/ssl/roots.der - корневые сертификаты системы */
static i64 xsend(i64 h, const void* p, u32 n) { return use_tls ? tls_send(&tls, p, n) : k_net_send(h, p, n); }
static i64 xrecv(i64 h, void* p, u32 n, u32 ms) { return use_tls ? tls_recv(&tls, p, n, ms) : k_net_recv(h, p, n, ms); }

static char buf[65536];
static tls_ticket_t saved;                         /* билет первого соединения для второго (-r) */
static int session(const char* host, u32 port, const char* path, i64 ip, const tls_ticket_t* tk);
static void say(const char* s) { k_write(s, k_strlen(s)); }
static void say_u(u64 v) { char t[21]; int i = 20; t[i] = 0; do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v); say(t + i); }

int umain(void) {
    char a[128];
    if (k_args(a, sizeof a) <= 0) { say("fetch: usage: fetch [-r] [https://]host[:port] [path]\n"); return 1; }
    char host[96]; u32 n = 0, port = 80;
    const char* p = a;
    int again = 0;                                                /* -r: повторить по билету сессии */
    if (p[0] == '-' && p[1] == 'r' && p[2] == ' ') { again = 1; p += 3; while (*p == ' ') p++; }
    if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p') {
        if (p[4] == 's' && p[5] == ':' && p[6] == '/' && p[7] == '/') { use_tls = 1; port = 443; p += 8; }
        else if (p[4] == ':' && p[5] == '/' && p[6] == '/') p += 7;
    }
    while (*p && *p != ' ' && *p != ':' && *p != '/' && n + 1 < sizeof host) host[n++] = *p++;
    host[n] = 0;
    if (*p == ':') { p++; port = 0; while (*p >= '0' && *p <= '9') port = port * 10 + (u32)(*p++ - '0'); }
    while (*p == ' ') p++;
    const char* path = *p ? p : "/";
    i64 ip = k_net_dns(host);
    if (ip < 0) { say("fetch: address not found: "); say(host); say("\n"); return 1; }
    int rc = session(host, port, path, ip, 0);
    if (rc == 0 && again && use_tls) {
        if (!saved.valid) { say("fetch: -r: the server sent no session ticket, nothing to resume\n"); return 1; }
        say("fetch: second connection with the session ticket\n");
        rc = session(host, port, path, ip, &saved);
    }
    return rc;
}

/* Одно соединение: TCP, (TLS), запрос, чтение до закрытия, итог. tk - билет прошлого соединения или 0. */
static int session(const char* host, u32 port, const char* path, i64 ip, const tls_ticket_t* tk) {
    u64 t0 = k_clock_ns();
    i64 h = k_net_connect((u32)ip, port);
    if (h < 0) { say("fetch: connection refused or no network\n"); return 1; }
    if (use_tls) {
        i64 rl = 0, sz = k_sys_stat("roots.der");
        if (sz > 0 && (u64)sz <= sizeof roots)
            while (rl < sz) { i64 r = k_sys_read("roots.der", roots + rl, (u64)(sz - rl), (u64)rl); if (r <= 0) break; rl += r; }
        if (rl != sz || sz <= 0) {                                /* без корневых личность сервера не проверить: отказ */
            say("fetch: no root certificates (/etc/ssl/roots.der): the server identity cannot be checked, not connecting\n");
            k_net_close(h); return 1;
        }
        i64 now = k_time();
        if (tls_connect(&tls, h, host, 10000, roots, (u32)rl, now > 0 ? now : 0, tk)) {
            say("fetch: TLS failed: "); say(tls.err); say("\n"); k_net_close(h); return 1;
        }
        say(tls.cipher == 0x1301 ? "fetch: TLS 1.3, X25519 + AES-128-GCM" : "fetch: TLS 1.3, X25519 + ChaCha20-Poly1305");
        if (tls.resumed) say("; session resumed from a ticket (psk_dhe_ke): no certificates sent, identity checked by the first connection");
        else {
            say("; certificate verified (chain to a root, name");
            say(now > 0 ? ", dates)" : "; dates NOT checked: no real-time clock)");
        }
        if (tls.weak_random) say("; key from timing-only randomness (no hardware RNG)");
        say("\n");
        if (!tls.resumed) {
            if (!tls.ocsp_stapled) say("fetch: OCSP: no stapled response (not an error)\n");
            else if (tls.ocsp) say("fetch: OCSP: stapled response checked (signature, certificate, dates): status good\n");
            else { say("fetch: OCSP: stapled response gives no guarantee: "); say(tls.ocsp_why ? tls.ocsp_why : "?"); say("\n"); }
        }
    }
    char req[256]; u32 k = 0;
    const char* parts[] = { "GET ", path, " HTTP/1.0\r\nHost: ", host, "\r\nUser-Agent: KiraOS-fetch\r\nConnection: close\r\n\r\n" };
    for (u32 i = 0; i < 5; i++) for (const char* c = parts[i]; *c && k + 1 < sizeof req; c++) req[k++] = *c;
    if (xsend(h, req, k) != (i64)k) { say("fetch: request not sent\n"); k_net_close(h); return 1; }
    u64 total = 0; i64 r;
    while ((r = xrecv(h, buf + (total < sizeof buf ? total : sizeof buf - 1024), 1024, 10000)) > 0) {
        if (total + (u64)r <= sizeof buf) total += (u64)r; else total = sizeof buf;
    }
    if (use_tls) { saved = tls.ticket; tls_close(&tls); }
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
