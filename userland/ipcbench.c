/* Замер обмена между процессами: «как было» (опрос портов по кругу, данные кусками внутри сообщений) против «как стало»
   (ожидание нескольких портов одним вызовом, данные одним вложением общей памяти). Два экземпляра: первый - сервер, второй - клиент. */
#include "kira.h"
KIRA_ENTRY()

static void say(const char* s) { k_write(s, k_strlen(s)); }
static void say_u(u64 v) { char t[21]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); char o[21]; int i = 0; while (n) o[i++] = t[--n]; k_write(o, (u64)i); }
#define N 400            /* сообщений на каждый замер */
#define R 8              /* повторов передачи 1 МиБ */
#define MIB (1u << 20)
#define CH 65536u        /* кусок внутри сообщения: предел сообщения порта */

static i64 pa, pb, ps, rep;
static u8 buf[CH];

/* Сервер: получает от клиента сообщение по любому из двух портов и отвечает байтом. */
static int got_poll(void) {                       /* как было: по кругу спрашиваем каждый порт без ожидания, между кругами уступаем процессор */
    u64 spins = 0;
    for (;;) {
        i64 r = k_port_recv(pa, buf, 8, 0);
        if (r < 0) r = k_port_recv(pb, buf, 8, 0);
        if (r >= 0) return (int)spins;
        spins++;
        k_yield();
    }
}
static int got_wait(void) {                       /* как стало: один вызов ждёт оба порта */
    int hs[2] = { (int)pa, (int)pb };
    i64 w = k_port_wait(hs, 2, 0xFFFFFFFFu);
    if (w < 0) return -1;
    k_port_recv(hs[w], buf, 8, 0);
    return 0;
}
static u64 sum(const u8* p, u32 n) { u64 s = 0; for (u32 i = 0; i < n; i++) s += p[i]; return s; }

static int server(void) {
    u8 ack = 1;
    i64 r = -1;
    for (int i = 0; i < 200 && r < 0; i++) { r = k_port_open("bench.rep", 0); if (r < 0) k_sleep(1); }
    if (r < 0) { say("ipcbench: FAIL no reply port\n"); return 1; }
    rep = r;
    u64 spins = 0;
    for (int i = 0; i < N; i++) { spins += (u64)got_poll(); k_port_send(rep, &ack, 1); }      /* 1: опрос */
    say("ipcbench: server poll: "); say_u(spins); say(" idle polling rounds over "); say_u(N); say(" messages\n");
    for (int i = 0; i < N; i++) { if (got_wait() < 0) { say("ipcbench: FAIL wait\n"); return 1; } k_port_send(rep, &ack, 1); }   /* 2: ожидание */
    for (int i = 0; i < N; i++) { if (k_port_recv(pa, buf, 8, 0xFFFFFFFFu) < 0) return 1; k_port_send(rep, &ack, 1); }          /* 3: один порт */
    spins = (u64)got_poll(); k_port_send(rep, &ack, 1);                                         /* 4: простой, опрос */
    say("ipcbench: server idle 300 ms by polling: "); say_u(spins); say(" rounds (each round = 2 calls + yield)\n");
    if (got_wait() < 0) return 1; k_port_send(rep, &ack, 1);                                    /* 5: простой, ожидание: один вызов */
    say("ipcbench: server idle 300 ms by waiting: 1 call, 0 rounds\n");
    u64 chk = 0;
    for (int rr = 0; rr < R; rr++) {                                                            /* 6: 1 МиБ кусками в сообщениях */
        for (u32 got = 0; got < MIB; ) { i64 n = k_port_recv(ps, buf, CH, 0xFFFFFFFFu); if (n < 0) return 1; u32 len = (u32)n; chk += sum(buf, len); got += len; }
        k_port_send(rep, &ack, 1);
    }
    for (int rr = 0; rr < R; rr++) {                                                            /* 7: 1 МиБ одним вложением памяти */
        int att = 0; i64 n = k_port_recv_att(ps, buf, 8, 0xFFFFFFFFu, 0, &att);
        if (n < 0 || att <= 0) return 1;
        u8* m = (u8*)k_shm_map(att);
        if (!m) return 1;
        chk += sum(m, MIB);
        k_shm_unmap(m); k_close(att);
        k_port_send(rep, &ack, 1);
    }
    say("ipcbench: server done, checksum "); say_u(chk); say("\n");
    return 0;
}

static void line(const char* what, u64 ns, u64 per) { say("ipcbench: "); say(what); say(": "); say_u(ns / 1000); say(" us total, "); say_u(per); say(" us each\n"); }

static int client(void) {
    ps = -1;
    for (int i = 0; i < 200 && (pa < 0 || pb < 0 || ps < 0); i++) {
        pa = k_port_open("bench.a", 0); pb = k_port_open("bench.b", 0); ps = k_port_open("bench.s", 0);
        if (pa < 0 || pb < 0 || ps < 0) k_sleep(1);
    }
    if (pa < 0 || pb < 0 || ps < 0) { say("ipcbench: FAIL client ports\n"); return 1; }
    u8 m = 7, a[4];
    for (int ph = 1; ph <= 3; ph++) {
        u64 t0 = k_clock_ns();
        for (int i = 0; i < N; i++) {
            i64 to = ph == 3 ? pa : (i & 1 ? pb : pa);
            if (k_port_send(to, &m, 1) != 0 || k_port_recv(rep, a, 4, 500) < 0) { say("ipcbench: FAIL exchange\n"); return 1; }
        }
        u64 d = k_clock_ns() - t0;
        line(ph == 1 ? "round trip, polling two ports (before)" : ph == 2 ? "round trip, waiting on two ports (after)" : "round trip, one port (floor)", d, d / 1000 / N);
    }
    for (int ph = 4; ph <= 5; ph++) {                                                           /* простой: сообщение приходит через ~300 мс */
        k_sleep(30);
        k_port_send(pa, &m, 1);
        if (k_port_recv(rep, a, 4, 500) < 0) { say("ipcbench: FAIL idle\n"); return 1; }
    }
    static u8 chunk[CH];
    for (u32 i = 0; i < CH; i++) chunk[i] = (u8)i;
    u64 t0 = k_clock_ns();
    for (int rr = 0; rr < R; rr++) {
        for (u32 off = 0; off < MIB; off += CH) { chunk[0] = (u8)(rr + off); if (k_port_send(ps, chunk, CH) != 0) { say("ipcbench: FAIL send chunk\n"); return 1; } }
        if (k_port_recv(rep, a, 4, 1000) < 0) { say("ipcbench: FAIL chunks ack\n"); return 1; }
    }
    u64 d1 = k_clock_ns() - t0;
    line("1 MiB in 64 KiB messages (before)", d1, d1 / 1000 / R);
    i64 shm = k_shm_open("bench.shm", MIB / 4096, KOPEN_CREATE);
    u8* mem = shm < 0 ? 0 : (u8*)k_shm_map(shm);
    if (!mem) { say("ipcbench: FAIL shm\n"); return 1; }
    t0 = k_clock_ns();
    for (int rr = 0; rr < R; rr++) {
        for (u32 off = 0; off < MIB; off += CH) { chunk[0] = (u8)(rr + off); __builtin_memcpy(mem + off, chunk, CH); }   /* те же 1 МиБ данных кладём в память */
        if (k_port_send_att(ps, shm, &m, 1) != 0) { say("ipcbench: FAIL send att\n"); return 1; }
        if (k_port_recv(rep, a, 4, 1000) < 0) { say("ipcbench: FAIL att ack\n"); return 1; }
    }
    u64 d2 = k_clock_ns() - t0;
    line("1 MiB by one shared-memory attachment (after)", d2, d2 / 1000 / R);
    say("ipcbench: done\n");
    return 0;
}

int umain(void) {
    pa = k_port_open("bench.a", KOPEN_CREATE | KOPEN_EXCL | KOPEN_PUBLIC);
    if (pa >= 0) {
        pb = k_port_open("bench.b", KOPEN_CREATE | KOPEN_PUBLIC);
        ps = k_port_open("bench.s", KOPEN_CREATE | KOPEN_PUBLIC);
        if (pb < 0 || ps < 0) { say("ipcbench: FAIL ports\n"); return 1; }
        return server();
    }
    pa = pb = ps = -1;
    rep = k_port_open("bench.rep", KOPEN_CREATE | KOPEN_PUBLIC);
    if (rep < 0) { say("ipcbench: FAIL reply port\n"); return 1; }
    return client();
}
