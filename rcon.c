#include "rcon.h"
#include "sha256.h"
#ifndef KIRA_HOST_TEST
#include "console.h"
#include "klog.h"
#include "net.h"
#include "rng.h"
#include "sched.h"
#include "timer.h"
#include "uart.h"
#include "vfs.h"
#endif

#define KEY_FILE  "/var/rcon.key"
#define CONF_FILE "/var/rcon.conf"
#define STRETCH   4096
#define MAX_FAILS 5
#define LOCK_SECS 60

void rcon_make_key(const uint8_t salt[16], const char* pass, uint8_t key[RCON_KEY_LEN]) {
    uint32_t pl = 0; while (pass[pl]) pl++;
    uint8_t h[32];
    sha256_ctx_t c;
    sha256_init(&c); sha256_update(&c, "kira-rcon-v1", 13); sha256_update(&c, salt, 16); sha256_update(&c, pass, pl); sha256_final(&c, h);
    for (int i = 0; i < STRETCH; i++) { sha256_init(&c); sha256_update(&c, h, 32); sha256_update(&c, salt, 16); sha256_update(&c, pass, pl); sha256_final(&c, h); }
    for (int i = 0; i < 16; i++) key[i] = salt[i];
    for (int i = 0; i < 32; i++) key[16 + i] = h[i];
}

int rcon_check_pass(const uint8_t key[RCON_KEY_LEN], const char* pass) {
    uint8_t k2[RCON_KEY_LEN];
    rcon_make_key(key, pass, k2);
    uint8_t d = 0;
    for (int i = 0; i < RCON_KEY_LEN; i++) d |= (uint8_t)(k2[i] ^ key[i]);
    return d == 0;
}

const char* rcon_parse_auth(char* line) {
    unsigned n = 0; while (line[n]) n++;
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = 0;
    if (n < 6 || line[0] != 'A' || line[1] != 'U' || line[2] != 'T' || line[3] != 'H' || line[4] != ' ') return 0;
    return line + 5;
}

#ifndef KIRA_HOST_TEST
static struct {
    volatile int on, stop;
    uint16_t port;
    int have_key; uint8_t key[RCON_KEY_LEN];
    int listener; uint32_t fails; uint64_t lock_until;
    uint64_t sessions, denied; int active;
    int loaded;
} R = { .port = RCON_PORT_DEFAULT, .listener = -1 };

static int send_str(int h, const char* s) {
    uint32_t n = 0; while (s[n]) n++;
    return tcp_send(h, s, n) < 0 ? -1 : 0;
}

static void conf_save(void) {
    char b[16]; int n = 0;
    b[n++] = R.on ? '1' : '0'; b[n++] = ' ';
    char t[8]; int k = 0; unsigned v = R.port; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) b[n++] = t[--k];
    b[n++] = '\n';
    vfs_mkdir("/var");
    vfs_write(CONF_FILE, b, (size_t)n);
}

static void load_files(void) {
    uint8_t k[RCON_KEY_LEN];
    if (vfs_read(KEY_FILE, 0, k, sizeof k) == (int64_t)sizeof k) { for (int i = 0; i < RCON_KEY_LEN; i++) R.key[i] = k[i]; R.have_key = 1; }
    char b[16] = { 0 };
    int64_t n = vfs_read(CONF_FILE, 0, b, 15);
    if (n >= 3 && b[1] == ' ') {
        unsigned p = 0; for (int i = 2; i < n && b[i] >= '0' && b[i] <= '9'; i++) p = p * 10 + (unsigned)(b[i] - '0');
        if (p > 0 && p < 65536) R.port = (uint16_t)p;
        if (b[0] == '1' && R.have_key) R.on = 1;
    }
    R.loaded = 1;
}

/* Сессия: проверка пароля, затем строки клиента - в консоль, журнал ядра - клиенту. */
static void session(int h) {
    char line[160]; unsigned ll = 0;
    if (send_str(h, "KIRA-RCON 1\n")) return;
    int authed = 0;
    uint64_t deadline = timer_get_ticks() + 20ull * TIMER_HZ;
    uint64_t cursor = 0;
    for (;;) {
        if (!R.on || R.stop) { send_str(h, "BYE\n"); return; }
        char c;
        int n = tcp_recv(h, &c, 1, 1);
        if (n == 0 || n == -2) return;
        if (n == 1) {
            if (c == '\n' || ll >= sizeof line - 1) {
                line[ll] = 0; ll = 0;
                if (!authed) {
                    const char* pw = rcon_parse_auth(line);
                    if (pw && rcon_check_pass(R.key, pw)) {
                        authed = 1; R.fails = 0; R.active = 1;
                        cursor = klog_total();
                        uart_print("[KIRA RCON] client logged in\n");
                        if (send_str(h, "OK\n")) return;
                    } else {
                        R.denied++;
                        if (++R.fails >= MAX_FAILS) { R.lock_until = timer_get_ticks() + (uint64_t)LOCK_SECS * TIMER_HZ; R.fails = 0; uart_print("[KIRA RCON] too many wrong passwords: locked for a minute\n"); }
                        sched_sleep_ticks(TIMER_HZ);                       /* медленный перебор */
                        send_str(h, "ERR\n");
                        return;
                    }
                } else {
                    unsigned e = 0; while (line[e] && line[e] != '\r') e++;
                    line[e] = 0;
                    if (line[0] == 'Q' && line[1] == 'U' && line[2] == 'I' && line[3] == 'T' && !line[4]) { send_str(h, "BYE\n"); R.active = 0; return; }
                    if (line[0] == 'P' && line[1] == 'I' && line[2] == 'N' && line[3] == 'G' && !line[4]) { if (send_str(h, "PONG\n")) { R.active = 0; return; } }
                    else if (line[0]) {
                        int quiet = line[0] == 'r' && line[1] == 'c' && line[2] == 'o' && line[3] == 'n' && line[4] == ' ' && line[5] == 'p';   /* «rcon passwd» не в журнал */
                        if (!quiet) { uart_print("[KIRA RCON] command: "); uart_print(line); uart_print("\n"); }
                        if (console_request(line) != 0 && send_str(h, "BUSY\n")) { R.active = 0; return; }
                    }
                }
            } else if (c != '\r') line[ll++] = c;
        }
        if (!authed) { if (timer_get_ticks() > deadline) return; continue; }
        char out[512]; unsigned lost = 0;
        for (;;) {                                                           /* вывод ядра - клиенту */
            unsigned got = klog_read(&cursor, out, sizeof out, &lost);
            if (!got) break;
            if (tcp_send(h, out, got) < 0) { R.active = 0; return; }
        }
    }
}

static void rcon_main(void* arg) {
    (void)arg;
    while (!vfs_any()) sched_sleep_ticks(TIMER_HZ);
    load_files();
    for (;;) {
        if (!R.on || !net_dev()) { if (R.listener >= 0) { tcp_unlisten(R.listener); R.listener = -1; } sched_sleep_ticks(TIMER_HZ / 2 ? TIMER_HZ / 2 : 1); continue; }
        if (R.listener < 0) {
            R.listener = tcp_listen(R.port);
            if (R.listener < 0) { sched_sleep_ticks(TIMER_HZ * 2); continue; }
            uart_print("[KIRA RCON] listening on TCP port "); uart_print_dec(R.port); uart_print("\n");
        }
        int h = tcp_accept(R.listener, TIMER_HZ / 2 ? TIMER_HZ / 2 : 1);
        if (h < 0) continue;
        if (timer_get_ticks() < R.lock_until) { R.denied++; tcp_close(h); continue; }
        R.sessions++;
        session(h);
        R.active = 0;
        tcp_close(h);
    }
}

void rcon_init(void) {
    if (!sched_create("rcon", rcon_main, 0, PRIO_NORMAL)) uart_print("[KIRA RCON] cannot create thread.\n");
}

static void usage(void) {
    uart_print("rcon: удалённая консоль по TCP (выключена, пока не включить)\n"
               "  rcon passwd ПАРОЛЬ   задать пароль (от 8 знаков; он виден в терминале при вводе)\n"
               "  rcon on [порт]       включить (порт по умолчанию 7077; нужен пароль); включение сохраняется\n"
               "  rcon off             выключить\n"
               "  rcon status          состояние\n"
               "Клиент: nc адрес порт, первая строка «AUTH пароль». Пароль идёт открытым текстом: только доверенная сеть.\n");
}

static int word_is(const char* w, unsigned wl, const char* s) {
    unsigned i = 0;
    while (s[i] && i < wl && w[i] == s[i]) i++;
    return !s[i] && i == wl;
}

void rconcmd(const char* arg) {
    while (arg && *arg == ' ') arg++;
    if (!arg || !*arg) { usage(); return; }
    const char* w = arg; unsigned wl = 0; while (w[wl] && w[wl] != ' ') wl++;
    const char* rest = w + wl; while (*rest == ' ') rest++;
    #define IS(s) word_is(w, wl, s)
    if (IS("passwd")) {
        unsigned n = 0; while (rest[n]) n++;
        if (n < 8) { uart_print("rcon: пароль короче 8 знаков\n"); return; }
        uint8_t salt[16]; rng_get(salt, 16);
        rcon_make_key(salt, rest, R.key);
        vfs_mkdir("/var");
        if (vfs_write(KEY_FILE, R.key, RCON_KEY_LEN) != 0) { uart_print("rcon: не удалось сохранить пароль на диск (работает до перезагрузки)\n"); }
        R.have_key = 1;
        uart_print("rcon: пароль задан\n");
    } else if (IS("on")) {
        if (!R.have_key) { uart_print("rcon: сначала задайте пароль: rcon passwd ПАРОЛЬ\n"); return; }
        if (*rest) { unsigned p = 0; for (const char* q = rest; *q >= '0' && *q <= '9'; q++) p = p * 10 + (unsigned)(*q - '0'); if (p == 0 || p > 65535) { uart_print("rcon: порт 1..65535\n"); return; } if (p != R.port && R.listener >= 0) { tcp_unlisten(R.listener); R.listener = -1; } R.port = (uint16_t)p; }
        R.on = 1; conf_save();
        uart_print("rcon: включена, порт "); uart_print_dec(R.port); uart_print("\n");
    } else if (IS("off")) {
        R.on = 0; conf_save();
        uart_print("rcon: выключена\n");
    } else if (IS("status")) {
        uart_print("rcon: "); uart_print(R.on ? "включена" : "выключена"); uart_print(", порт "); uart_print_dec(R.port);
        uart_print(R.have_key ? ", пароль задан" : ", пароля нет");
        uart_print(", сеанс "); uart_print(R.active ? "идёт" : "нет"); uart_print(", сеансов "); uart_print_dec(R.sessions);
        uart_print(", отказов "); uart_print_dec(R.denied);
        if (timer_get_ticks() < R.lock_until) uart_print(", вход временно заблокирован");
        uart_print("\n");
    } else usage();
}
#endif
