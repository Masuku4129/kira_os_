/* Проверка обмена между процессами: два экземпляра. Первый занимает порт kira.test (сервер), второй - клиент.
   Клиент шлёт 200 сообщений и ждёт ответа на каждое через свой порт; оба работают с общей страницей памяти. */
#include "kira.h"
#include "kmsg.h"      /* сообщения портов - детерминированный CBOR, строгий разбор (cbor.c - тот же файл, что у ядра) */
KIRA_ENTRY()

static void say(const char* s) { k_write(s, k_strlen(s)); }
static void say_u(u64 v) { char t[21]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); char o[21]; int i = 0; while (n) o[i++] = t[--n]; k_write(o, (u64)i); }
#define N 200

static int server(i64 port) {
    /* Keep foreign private objects alive while the client attempts to open them. */
    i64 private_port = k_port_open("kira.test.private", KOPEN_CREATE);
    i64 private_shm = k_shm_open("kira.test.private.shm", 1, KOPEN_CREATE);
    if (private_port < 0 || private_shm < 0) {
        say("ipc server: FAIL private object creation\n");
        return 1;
    }
    i64 shm = k_shm_open("kira.test.shm", 1, KOPEN_CREATE | KOPEN_PUBLIC);
    volatile u32* page = shm < 0 ? (volatile u32*)0 : (volatile u32*)k_shm_map(shm);
    if (!page) { say("ipc server: FAIL shared memory\n"); return 1; }
    page[0] = 0x4B495241;                                       /* метка для клиента */
    __atomic_store_n(&page[1], 1, __ATOMIC_RELEASE);
    k_futex_wake(&page[1], 1);
    say("ipc server: waiting on port kira.test\n");
    i64 reply = -1;
    u32 got = 0, rejected = 0;
    u8 m[64], o[32];
    for (;;) {
        i64 r = k_port_recv(port, m, sizeof m, 1000);            /* 10 с */
        if (r < 0) { say("ipc server: FAIL receive "); say_u((u64)-r); say("\n"); return 1; }
        char op[8]; uint64_t n = 0;
        int pe = msg_parse(m, (u32)r, op, &n);
        if (!pe && op[0] == 'd' && op[1] == 'o' && op[2] == 'n' && op[3] == 'e' && !op[4]) break;
        if (reply < 0) reply = k_port_open("kira.test.reply", 0);
        u32 ol;
        if (pe) { rejected++; ol = msg_make(o, sizeof o, "err", (u64)pe, 1); }                     /* мусор - ответ «err», сервер жив */
        else if (op[0] == 'p' && op[1] == 'i' && op[2] == 'n' && op[3] == 'g' && !op[4]) { ol = msg_make(o, sizeof o, "pong", n, 1); got++; }
        else { rejected++; ol = msg_make(o, sizeof o, "err", E_NOOP, 1); }
        if (reply < 0 || !ol || k_port_send(reply, o, ol) != 0) { say("ipc server: FAIL reply\n"); return 1; }
    }
    int ok = got == N && rejected == 3 && page[2] == 0x434C4945;   /* три испорченных сообщения клиента отвергнуты */                  /* клиент записал свою метку */
    say("ipc server: "); say_u(got); say(" requests answered, "); say_u(rejected); say(" malformed rejected, client mark in shared memory ");
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
    /* The server owns these names. Wait for publication, then verify both are denied. */
    i64 private_port = KE_NOENT, private_shm = KE_NOENT;
    for (int i = 0; i < 100 && private_port == KE_NOENT; i++) {
        private_port = k_port_open("kira.test.private", 0);
        if (private_port == KE_NOENT) k_sleep(1);
    }
    for (int i = 0; i < 100 && private_shm == KE_NOENT; i++) {
        private_shm = k_shm_open("kira.test.private.shm", 0, 0);
        if (private_shm == KE_NOENT) k_sleep(1);
    }
    int private_denied = private_port == KE_PERM && private_shm == KE_PERM;
    if (private_port >= 0) k_close(private_port);
    if (private_shm >= 0) k_close(private_shm);
    volatile u32* page = shm < 0 ? (volatile u32*)0 : (volatile u32*)k_shm_map(shm);
    if (port < 0 || reply < 0 || !page || !private_denied) {
        say("ipc client: FAIL setup/private isolation\n");
        return 1;
    }
    say("ipc client: foreign private port and SHM denied (PASS)\n");
    while (!__atomic_load_n(&page[1], __ATOMIC_ACQUIRE)) k_futex_wait(&page[1], 0, 100);   /* futex через общую память */
    if (page[0] != 0x4B495241) { say("ipc client: FAIL shared memory content\n"); return 1; }
    u64 t0 = k_clock_ns();
    int bad = 0;
    for (u32 i = 0; i < N && !bad; i++) {
        u8 m[32], r[32]; u32 p = msg_make(m, sizeof m, "ping", i, 1);
        if (!p || k_port_send(port, m, p) != 0) { bad = 1; break; }
        i64 rr = k_port_recv(reply, r, sizeof r, 500);
        char op[8]; uint64_t n = 0;
        if (rr < 0 || msg_parse(r, (u32)rr, op, &n) != 0 || op[0] != 'p' || op[1] != 'o' || n != i) bad = 1;
    }
    /* испорченные сообщения: не CBOR, нестрогое кодирование числа (0x18 0x05), карта без "op" - каждое должно вернуть "err" */
    static const u8 junk[3][16] = { { 0xFF, 0x00, 0x13, 0x37 },
        { 0xA2, 0x61, 'n', 0x18, 0x05, 0x62, 'o', 'p', 0x64, 'p', 'i', 'n', 'g' },      /* число 5 в нестрогой форме */
        { 0xA1, 0x61, 'n', 0x01 } };                                                    /* нет "op" */
    static const u8 junk_len[3] = { 4, 13, 4 };
    static const u8 junk_code[3] = { E_FORMAT, E_FORMAT, E_NOOP };
    for (int j = 0; j < 3 && !bad; j++) {
        u8 r[32]; cbor_item it;
        if (k_port_send(port, junk[j], junk_len[j]) != 0) { bad = 1; break; }
        i64 rr = k_port_recv(reply, r, sizeof r, 500);
        if (rr < 0 || cbor_check(r, (u32)rr, 1) != CBOR_OK || cbor_map_find(r, (u32)rr, "code", &it, 0) != CBOR_OK ||
            it.type != CBOR_UINT || it.val != junk_code[j]) bad = 1;
    }
    u64 us = (k_clock_ns() - t0) / 1000;
    page[2] = 0x434C4945;
    { u8 d[16]; u32 dl = msg_make(d, sizeof d, "done", 0, 0); k_port_send(port, d, dl); }
    say("ipc client: "); say_u(N); say(" round trips in "); say_u(us / 1000); say(" ms ("); say_u(us / N); say(" us each)");
    say(bad ? " - FAIL\n" : " - PASS\n");
    return bad;
}

int umain(void) {
    i64 port = k_port_open("kira.test", KOPEN_CREATE | KOPEN_EXCL | KOPEN_PUBLIC);
    if (port >= 0) return server(port);
    if (port == KE_EXIST) return client();
    say("ipc: FAIL cannot open port\n");
    return 1;
}
