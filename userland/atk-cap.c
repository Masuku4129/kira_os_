/* Проверка прав (caps.h): у программы нет ни одного права, поэтому каждый вызов, требующий права, обязан вернуть отказ
   (-12 или -100 для файлов), а безобидные вызовы (часы, pid, куча, случайные числа) - работать. Обе архитектуры. */
#include "kira.h"
static void say(const char* s) { k_write(s, k_strlen(s)); }
static void put(i64 v) { char b[24]; int n = 0; u64 x = v < 0 ? (u64)-v : (u64)v; do { b[n++] = (char)('0' + x % 10); x /= 10; } while (x); if (v < 0) b[n++] = '-'; char o[26]; int k = 0; while (n) o[k++] = b[--n]; o[k] = 0; say(o); }

KIRA_ENTRY()
int umain(void) {
    struct { const char* name; u64 nr; u64 a1, a2, a3; i64 want; } t[] = {
        { "gfx_create",   K_GFX_CREATE,  64, 64, 0, -12 },
        { "gfx_event",    K_GFX_EVENT,   0, 0, 0, -12 },
        { "net_dns",      K_NET_DNS,     0, 0, 0, -12 },
        { "net_connect",  K_NET_CONNECT, 0, 80, 0, -12 },
        { "net_listen",   K_NET_LISTEN,  8080, 0, 0, -12 },
        { "udp_open",     K_UDP_OPEN,    9000, 0, 0, -12 },
        { "snd_open",     K_SND_OPEN,    0, 0, 0, -12 },
        { "snd_volume",   K_SND_VOLUME,  0, 50, 0, -12 },
        { "snd_record",   K_SND_RECORD,  0, 0, 0, -12 },
        { "port_open",    K_PORT_OPEN,   0, 0, 0, -12 },
        { "shm_open",     K_SHM_OPEN,    0, 0, 0, -12 },
        { "thread_create", K_THREAD_CREATE, 0, 0, 0, -12 },
    };
    unsigned bad = 0, n = sizeof t / sizeof t[0];
    for (unsigned i = 0; i < n; i++) {
        i64 r = ksys(t[i].nr, t[i].a1, t[i].a2, t[i].a3);
        if (r != t[i].want) { say("atk-cap: "); say(t[i].name); say(" returned "); put(r); say(" instead of denial (FAIL)\n"); bad++; }
    }
    i64 fs = k_fs(1, "x", 1, 0, 0, 0);                          /* файл песочницы */
    if (fs != -100) { say("atk-cap: fs sandbox returned "); put(fs); say(" instead of -100 (FAIL)\n"); bad++; }
    u8 buf[8];
    if (k_sys_read("roots.der", buf, sizeof buf, 0) != -100) { say("atk-cap: /etc/ssl readable without right (FAIL)\n"); bad++; }
    if (k_getpid() == 0 || k_ticks() == 0 || k_heap_grow(1) == 0 || k_random(buf, sizeof buf) < 0) { say("atk-cap: harmless calls failed (FAIL)\n"); bad++; }
    if (bad) return 1;
    say("atk-cap: "); put(n + 2); say(" calls without rights refused, harmless calls work (PASS)\n");
    return 0;
}
