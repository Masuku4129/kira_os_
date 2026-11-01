/* udpecho [порт] [число] - UDP для программ: ждёт датаграммы на порту (по умолчанию 9000) и отвечает отправителю
   тем же текстом с приставкой «kira-udp: ». После «число» ответов (по умолчанию 3) или 30 с тишины - выход. */
#include "kira.h"
KIRA_ENTRY()

static void say(const char* s) { k_write(s, k_strlen(s)); }
static void say_u(u64 v) { char t[21]; int i = 20; t[i] = 0; do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v); say(t + i); }
static u32 num(const char** p, u32 def) { u32 v = 0; int any = 0; while (**p == ' ') (*p)++; while (**p >= '0' && **p <= '9') { v = v * 10 + (u32)(*(*p)++ - '0'); any = 1; } return any ? v : def; }

int umain(void) {
    char a[64]; const char* p = a;
    if (k_args(a, sizeof a) <= 0) a[0] = 0;
    u32 port = num(&p, 9000), count = num(&p, 3);
    i64 s = k_udp_open(port);
    if (s < 0) { say("udpecho: cannot open UDP port (no network or port busy)\n"); return 1; }
    say("udpecho: listening on UDP port "); say_u(port); say("\n");
    static u8 buf[8 + 1472], out[1500];
    u32 done = 0;
    while (done < count) {
        i64 n = k_udp_recv(s, buf, sizeof buf, 30000);
        if (n < 0) break;
        u32 ip = (u32)buf[0] << 24 | (u32)buf[1] << 16 | (u32)buf[2] << 8 | buf[3], from = (u32)buf[4] << 8 | buf[5];
        const char* pre = "kira-udp: "; u32 k = 0;
        while (pre[k]) { out[k] = (u8)pre[k]; k++; }
        for (i64 i = 0; i < n && k < sizeof out; i++) out[k++] = buf[8 + i];
        if (k_udp_send(s, ip, from, out, k) != (i64)k) { say("udpecho: send failed\n"); break; }
        done++;
        say("udpecho: "); say_u((u64)n); say(" bytes from "); say_u(ip >> 24); say("."); say_u((ip >> 16) & 255); say(".");
        say_u((ip >> 8) & 255); say("."); say_u(ip & 255); say(":"); say_u(from); say(", answered\n");
    }
    k_udp_close(s);
    say("udpecho: done, "); say_u(done); say(" answered\n");
    return 0;
}
