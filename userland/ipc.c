/* Проверка обмена между процессами: два экземпляра. Первый занимает порт kira.test (сервер), второй - клиент.
   Клиент шлёт 200 сообщений и ждёт ответа на каждое через свой порт; оба работают с общей страницей памяти. */
#include "kira.h"
KIRA_ENTRY()

static void say(const char* s) { k_write(s, k_strlen(s)); }
static void say_u(u64 v) { char t[21]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); char o[21]; int i = 0; while (n) o[i++] = t[--n]; k_write(o, (u64)i); }
#define N 200

static int server(i64 port) {
    i64 shm = k_shm_open("kira.test.shm", 1, KOPEN_CREATE | KOPEN_PUBLIC);
    volatile u32* page = shm < 0 ? (volatile u32*)0 : (volatile u32*)k_shm_map(shm);
    if (!page) { say("ipc server: FAIL shared memory\n"); return 1; }
    page[0] = 0x4B495241;                                       /* метка для клиента */
    __atomic_store_n(&page[1], 1, __ATOMIC_RELEASE);
    k_futex_wake(&page[1], 1);
    say("ipc server: waiting on port kira.test\n");
    i64 reply = -1;
    u32 got = 0;
    char m[32];
    for (;;) {
        i64 r = k_port_recv(port, m, sizeof m, 1000);            /* 10 с */
        if (r < 0) { say("ipc server: FAIL receive "); say_u((u64)-r); say("\n"); return 1; }
        u32 len = (u32)r;
        if (len == 4 && m[0] == 'd') break;                       /* "done" */
        if (reply < 0) reply = k_port_open("kira.test.reply", 0);
        m[0] = 'P';                                               /* ping -> Ping: ответ тем же числом */
        if (reply < 0 || k_port_send(reply, m, len) != 0) { say("ipc server: FAIL reply\n"); return 1; }
        got++;
    }
    int ok = got == N && page[2] == 0x434C4945;                  /* клиент записал свою метку */
    say("ipc server: "); say_u(got); say(" requests answered, client mark in shared memory ");
    say(page[2] == 0x434C4945 ? "seen" : "MISSING"); say(ok ? " - PASS\n" : " - FAIL\n");
    k_shm_unmap((void*)page);
    return ok ? 0 : 1;
}

static int client(void) {
    i64 port = -1;
    for (int i = 0; i < 100 && port < 0; i++) { port = k_port_open("kira.test", 0); if (port < 0) k_sleep(1); }
    i64 reply = k_port_open("kira.test.reply", KOPEN_CREATE | KOPEN_EXCL | KOPEN_PUBLIC);
    i64 shm = k_shm_open("kira.test.shm", 0, 0);
    for (int i = 0; i < 100 && shm < 0; i++) { k_sleep(1); shm = k_shm_open("kira.test.shm", 0, 0); }
    volatile u32* page = shm < 0 ? (volatile u32*)0 : (volatile u32*)k_shm_map(shm);
    if (port < 0) { say("ipc client: FAIL public port\n"); return 1; }
    i64 private_port = k_port_open("kira.test.private", 0);
    i64 private_shm = k_shm_open("kira.test.private.shm", 1, 0);
    if (private_port != KE_PERM || private_shm != KE_PERM) {
        say("ipc client: FAIL foreign PID opened private IPC object\n"); return 1;
    }
    say("ipc client: private port and shared memory denied to foreign PID (PASS)\n");
    if (reply < 0 || !page) { say("ipc client: FAIL setup\n"); return 1; }
    while (!__atomic_load_n(&page[1], __ATOMIC_ACQUIRE)) k_futex_wait(&page[1], 0, 100);   /* futex через общую память */
    if (page[0] != 0x4B495241) { say("ipc client: FAIL shared memory content\n"); return 1; }
    u64 t0 = k_clock_ns();
    int bad = 0;
    for (u32 i = 0; i < N && !bad; i++) {
        char m[16] = "ping "; u32 v = i, p = 5; char d[10]; int k = 0;
        do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (k) m[p++] = d[--k];
        if (k_port_send(port, m, p) != 0) { bad = 1; break; }
        char r[16];
        i64 rr = k_port_recv(reply, r, sizeof r, 500);
        if (rr < 0 || (u32)rr != p || r[0] != 'P') bad = 1;
        for (u32 j = 1; !bad && j < p; j++) if (r[j] != m[j]) bad = 1;
    }
    u64 us = (k_clock_ns() - t0) / 1000;
    page[2] = 0x434C4945;
    k_port_send(port, "done", 4);
    say("ipc client: "); say_u(N); say(" round trips in "); say_u(us / 1000); say(" ms ("); say_u(us / N); say(" us each)");
    say(bad ? " - FAIL\n" : " - PASS\n");
    return bad;
}

int umain(void) {
    i64 private_port = k_port_open("kira.test.private", KOPEN_CREATE | KOPEN_EXCL);
    if (private_port >= 0) {
        i64 private_shm = k_shm_open("kira.test.private.shm", 1, KOPEN_CREATE | KOPEN_EXCL);
        if (private_shm < 0) { say("ipc: FAIL create private shared memory\n"); return 1; }
        i64 port = k_port_open("kira.test", KOPEN_CREATE | KOPEN_EXCL | KOPEN_PUBLIC);
        if (port >= 0) return server(port);
        say("ipc: FAIL create public port\n");
        return 1;
    }
    if (private_port == KE_EXIST) return client();
    say("ipc: FAIL cannot create private port\n");
    return 1;
}
