/* Проверка управляющего слоя графического сервера на портах (gfxctl.c): порт kira.gfx, сообщения CBOR v1, ответы в личный порт клиента.
   Окна у программы нет: размер экрана известен, «win» и «events» отвечают кодом -2, лишний ключ и неизвестная операция отвергаются. */
#include "kira.h"
#include "kframe.h"
KIRA_ENTRY()

static void say(const char* s) { k_write(s, k_strlen(s)); }
static void say_u(u64 v) { char t[21]; int n = 0; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); char o[21]; int i = 0; while (n) o[i++] = t[--n]; k_write(o, (u64)i); }
static i64 svc, rep;
static u32 next_id = 1;
static u8 rq[128], rs[512];
static int checks, bad;
static void check(int ok, const char* what) { checks++; if (!ok) { bad++; say("gfxport: FAIL "); say(what); say("\n"); } }

static int rpc(const char* op, int att, kframe_t* f) {
    cbor_w w; cbor_w_init(&w, rq, sizeof rq);
    u32 id = next_id++;
    kf_req_begin(&w, id, op, 0);
    if (w.err || k_port_send_att(svc, att, rq, w.n) != 0) return 0;
    i64 n = k_port_recv(rep, rs, sizeof rs, 500);
    if (n <= 0 || kf_parse_resp(rs, (u32)n, f) != KF_OK) return 0;
    return f->id == id;
}

int umain(void) {
    for (int i = 0; i < 100 && svc <= 0; i++) { svc = k_port_open("kira.gfx", 0); if (svc < 0) { svc = 0; k_sleep(1); } }
    rep = k_port_open("gfxport.rep", KOPEN_CREATE);
    if (svc <= 0 || rep < 0) { say("gfxport: FAIL cannot open ports\n"); return 1; }
    kframe_t f;
    check(rpc("hello", (int)rep, &f) && f.code == 0, "hello");
    cbor_item it; u64 w = 0, h = 0;
    check(rpc("info", 0, &f) && f.code == 0 && f.body_len, "info");
    if (f.body_len) {
        check(cbor_map_find(f.body, f.body_len, "w", &it, 0) == CBOR_OK && it.type == CBOR_UINT && (w = it.val) >= 320, "info width");
        check(cbor_map_find(f.body, f.body_len, "h", &it, 0) == CBOR_OK && it.type == CBOR_UINT && (h = it.val) >= 200, "info height");
    }
    check(rpc("win", 0, &f) && f.code == -2, "win without a window = -2");
    check(rpc("events", 0, &f) && f.code == -2, "events without a window = -2");
    check(rpc("nosuch", 0, &f) && f.code == -101, "unknown op = -101");
    say("gfxport: screen "); say_u(w); say("x"); say_u(h); say(", ");
    say_u((u64)checks); say(" checks, "); say_u((u64)bad); say(bad ? " failed - FAIL\n" : " failed - PASS\n");
    return bad != 0;
}
