/* serve [порт] - маленький HTTP-сервер (программа Ring 3 / EL0) на сети ядра: принимает соединения, читает запрос,
   отвечает 200 с текстом (путь запроса и 20 КБ строк - ответ больше кольца отправки ядра, проверка окна TCP).
   По умолчанию порт 8080. Пример: serve 8080, затем с другой машины - http://адрес:8080/привет */
#include "kira.h"
KIRA_ENTRY()

static char body[22000];
static void say(const char* s) { k_write(s, k_strlen(s)); }
static void say_u(u64 v) { char t[21]; int i = 20; t[i] = 0; do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v); say(t + i); }
static u32 put(char* d, u32 n, const char* s) { while (*s) d[n++] = *s++; return n; }
static u32 put_u(char* d, u32 n, u64 v) { char t[21]; int i = 20; t[i] = 0; do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v); return put(d, n, t + i); }

int umain(void) {
    char a[32]; u32 port = 8080;
    if (k_args(a, sizeof a) > 0 && a[0] >= '0' && a[0] <= '9') { port = 0; for (char* p = a; *p >= '0' && *p <= '9'; p++) port = port * 10 + (u32)(*p - '0'); }
    i64 l = k_net_listen(port);
    if (l < 0) { say("serve: cannot listen (no network or port busy)\n"); return 1; }
    say("serve: listening on port "); say_u(port); say("\n");
    for (u64 served = 0;; ) {
        i64 h = k_net_accept(l, 0);
        if (h < 0) continue;
        char req[1024]; u32 got = 0;
        while (got < sizeof req - 1) {                           /* до конца заголовков */
            i64 r = k_net_recv(h, req + got, (u32)(sizeof req - 1 - got), 5000);
            if (r <= 0) break;
            got += (u32)r; req[got] = 0;
            int end = 0; for (u32 i = 3; i < got; i++) if (req[i - 3] == '\r' && req[i - 2] == '\n' && req[i - 1] == '\r' && req[i] == '\n') end = 1;
            if (end) break;
        }
        char path[128]; u32 pn = 0;
        if (got > 4 && req[0] == 'G' && req[1] == 'E' && req[2] == 'T' && req[3] == ' ')
            for (u32 i = 4; i < got && req[i] != ' ' && req[i] != '\r' && pn + 1 < sizeof path; i++) path[pn++] = req[i];
        path[pn] = 0;
        u32 n = put(body, 0, "kira-server-ok ");
        n = put(body, n, pn ? path : "?"); n = put(body, n, "\n");
        for (u32 i = 0; i < 900; i++) { n = put_u(body, n, 10000 + i); n = put(body, n, " kira tcp window\n"); }   /* 900 строк по 22 байта: 19,8 КБ */
        char hdr[128]; u32 hn = put(hdr, 0, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ");
        hn = put_u(hdr, hn, n); hn = put(hdr, hn, "\r\nConnection: close\r\n\r\n");
        i64 s1 = k_net_send(h, hdr, hn), s2 = k_net_send(h, body, n);
        k_net_close(h);
        served++;
        say("serve: GET "); say(pn ? path : "?"); say(" -> 200, "); say_u((u64)(s1 > 0 ? s1 : 0) + (u64)(s2 > 0 ? s2 : 0));
        say(" bytes sent (request "); say_u(served); say(")\n");
    }
}
