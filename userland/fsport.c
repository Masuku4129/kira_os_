/* Проверка файловой службы на портах (fssvc.c): порт kira.fs, сообщения CBOR v1, ответы в личный порт клиента (вложение),
   большие данные - через вложенную разделяемую память. Файлы - в песочнице программы /data/fsport. */
#include "kira.h"
#include "kframe.h"
KIRA_ENTRY()

static void say(const char* s) { k_write(s, k_strlen(s)); }
static void say_u(u64 v) { char t[21]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); char o[21]; int i = 0; while (n) o[i++] = t[--n]; k_write(o, (u64)i); }

#define FS_E_DENIED_ (-100)
#define FS_E_ARG_ (-101)
static int k_memcmp_(const void* a, const void* b, u64 n) { const u8* x = a; const u8* y = b; for (u64 i = 0; i < n; i++) if (x[i] != y[i]) return 1; return 0; }
static i64 svc, rep;
static u32 next_id = 1;
static u8 rq[512], rs[65536];
static int checks, bad;
static void check(int ok, const char* what) { checks++; if (!ok) { bad++; say("fsport: FAIL "); say(what); say("\n"); } }

/* Запрос: op и до двух аргументов (число и текст/байты) в карте args. Вложение att (0 - нет). Ответ разбирается в f; false - нет ответа. */
typedef struct { const char* name; u64 off, len; int has_off, has_len; const void* data; u32 dlen; } args_t;
static int rpc(const char* op, const args_t* a, int att, kframe_t* f, int junk) {
    cbor_w w; cbor_w_init(&w, rq, sizeof rq);
    u32 id = next_id++;
    if (junk) { rq[0] = 0xFF; rq[1] = 0x00; w.n = 2; w.err = 0; }
    else {
        kf_req_begin(&w, id, op, a != 0);
        if (a) {
            u32 np = (a->name ? 1 : 0) + a->has_off + a->has_len + (a->data ? 1 : 0);
            cbor_put_map(&w, np);                                         /* ключи по RFC 8949 4.2.1: короткие раньше, одной длины - по байтам */
            if (a->has_len) { cbor_put_cstr(&w, "len"); cbor_put_uint(&w, a->len); }
            if (a->has_off) { cbor_put_cstr(&w, "off"); cbor_put_uint(&w, a->off); }
            if (a->data) { cbor_put_cstr(&w, "data"); cbor_put_bytes(&w, a->data, a->dlen); }
            if (a->name) { cbor_put_cstr(&w, "name"); cbor_put_cstr(&w, a->name); }
        }
    }
    if (w.err) return 0;
    if (k_port_send_att(svc, att, rq, w.n) != 0) return 0;
    i64 n = k_port_recv(rep, rs, sizeof rs, 500);
    if (n <= 0) return 0;
    if (kf_parse_resp(rs, (u32)n, f) != KF_OK) return 0;
    return junk ? 1 : f->id == id;
}
static int rpc_code(const char* op, const args_t* a, int att, i64 want, const char* what) {
    kframe_t f;
    int ok = rpc(op, a, att, &f, 0) && f.code == want;
    check(ok, what);
    return ok;
}

int umain(void) {
    for (int i = 0; i < 100 && svc <= 0; i++) { svc = k_port_open("kira.fs", 0); if (svc < 0) { svc = 0; k_sleep(1); } }
    rep = k_port_open("fsport.rep", KOPEN_CREATE);                    /* личный порт: чужому по имени не открыть, службе он приходит вложением */
    if (svc <= 0 || rep < 0) { say("fsport: FAIL cannot open ports\n"); return 1; }
    kframe_t f;
    check(rpc("hello", 0, (int)rep, &f, 0) && f.code == 0, "hello");
    const char* text = "hello over ports";
    args_t w1 = { "note.txt", 0, 0, 0, 0, text, 16 };
    rpc_code("write", &w1, 0, 0, "write inline");
    args_t n1 = { "note.txt", 0, 0, 0, 0, 0, 0 };
    rpc_code("stat", &n1, 0, 16, "stat = 16");
    args_t r1 = { "note.txt", 0, 100, 0, 1, 0, 0 };
    int ok = rpc("read", &r1, 0, &f, 0) && f.code == 16 && f.body_len > 0;
    if (ok) { cbor_item it; cbor_r r; cbor_r_init(&r, f.body, f.body_len, 1); ok = cbor_next(&r, &it) == CBOR_OK && it.type == CBOR_BYTES && it.val == 16 && !k_memcmp_(it.data, text, 16); }
    check(ok, "read inline returns the written bytes");
    args_t r2 = { "note.txt", 6, 4, 1, 1, 0, 0 };
    ok = rpc("read", &r2, 0, &f, 0) && f.code == 4;
    if (ok) { cbor_item it; cbor_r r; cbor_r_init(&r, f.body, f.body_len, 1); ok = cbor_next(&r, &it) == CBOR_OK && it.type == CBOR_BYTES && it.val == 4 && !k_memcmp_(it.data, "over", 4); }
    check(ok, "read with offset");
    ok = rpc("list", 0, 0, &f, 0) && f.code >= 9 && f.body_len > 0;
    if (ok) { cbor_item it; cbor_r r; cbor_r_init(&r, f.body, f.body_len, 1); ok = cbor_next(&r, &it) == CBOR_OK && it.type == CBOR_BYTES && it.val >= 9; if (ok) { ok = 0; for (u64 i = 0; i + 9 <= it.val; i++) if (!k_memcmp_(it.data + i, "note.txt\n", 9)) ok = 1; } }
    check(ok, "list shows note.txt");

    /* большие данные - через вложенную память: 30000 байт туда и обратно */
    i64 sm = k_shm_open("fsport.buf", 8, KOPEN_CREATE);
    volatile u8* pg = sm < 0 ? (volatile u8*)0 : (volatile u8*)k_shm_map(sm);
    check(pg != 0, "shared memory");
    if (pg) {
        for (u32 i = 0; i < 30000; i++) pg[i] = (u8)(i * 7 + (i >> 8));
        args_t w2 = { "big.bin", 0, 30000, 0, 1, 0, 0 };
        rpc_code("write", &w2, (int)sm, 0, "write from shared memory");
        args_t n2 = { "big.bin", 0, 0, 0, 0, 0, 0 };
        rpc_code("stat", &n2, 0, 30000, "stat big.bin = 30000");
        for (u32 i = 0; i < 30000; i++) pg[i] = 0;
        args_t r3 = { "big.bin", 0, 30000, 0, 1, 0, 0 };
        rpc_code("read", &r3, (int)sm, 30000, "read into shared memory");
        int same = 1; for (u32 i = 0; i < 30000; i++) if (pg[i] != (u8)(i * 7 + (i >> 8))) { same = 0; break; }
        check(same, "30000 bytes came back unchanged");
        args_t r4 = { "big.bin", 0, 40000, 0, 1, 0, 0 };
        rpc_code("read", &r4, (int)sm, FS_E_ARG_, "read longer than the attached memory is refused");
        args_t d2 = { "big.bin", 0, 0, 0, 0, 0, 0 };
        rpc_code("remove", &d2, 0, 0, "remove big.bin");
        k_shm_unmap((void*)pg);
    }
    /* отказы: имя вне песочницы, неизвестная операция, испорченное сообщение */
    args_t bad1 = { "../etc/x", 0, 10, 0, 1, 0, 0 };
    rpc_code("read", &bad1, 0, FS_E_DENIED_, "name outside the sandbox is denied");
    args_t bad2 = { "a/b", 0, 0, 0, 0, 0, 0 };
    rpc_code("stat", &bad2, 0, FS_E_DENIED_, "slash in a name is denied");
    rpc_code("frob", 0, 0, FS_E_ARG_, "unknown operation");
    check(rpc("x", 0, 0, &f, 1) && f.id == 0 && f.code <= -201, "malformed message gets an error reply");
    rpc_code("remove", &n1, 0, 0, "remove note.txt");
    rpc_code("stat", &n1, 0, -2, "stat after remove = no such file");
    say("fsport: "); say_u((u64)checks); say(" checks, "); say_u((u64)bad); say(bad ? " failed - FAIL\n" : " failed - PASS\n");
    return bad ? 1 : 0;
}
