#include "fssvc.h"
#include "fsrv.h"
#include "kfs.h"
#include "kobj.h"
#include "proc.h"
#include "caps.h"
#include "heap.h"
#include "uart.h"
#include "userland/kframe.h"

#define NBIND 16
#define INLINE_MAX (48u * 1024)          /* данные внутри сообщения: остальное - через вложенную память */
#define SVC_CODE_FRAME (-200)            /* ошибка разбора сообщения: -200 - KF_E_* */

static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static kobj_t* svc;
static struct { uint32_t pid; kobj_t* rep; } bind[NBIND];
static uint8_t* mb;                       /* принятое сообщение */
static uint8_t* rb;                       /* ответ */

static kobj_t* bound(uint32_t pid) {
    for (unsigned i = 0; i < NBIND; i++) if (bind[i].rep && bind[i].pid == pid) return bind[i].rep;
    return 0;
}
static void unbind_dead(void) {
    for (unsigned i = 0; i < NBIND; i++) {
        char nm[16]; uint32_t c, d;
        if (bind[i].rep && !proc_info(bind[i].pid, nm, &c, &d)) { kobj_release(bind[i].rep); bind[i].rep = 0; }
    }
}

static void reply_code(kobj_t* rep, uint32_t id, int64_t code) {
    uint32_t n = kf_resp_code(rb, PORT_MSG_MAX, id, code);
    if (n) (void)port_send(rep, 0, rb, n);           /* очередь клиента полна - ответ теряется: клиент сам не читает */
}
static void reply_data(kobj_t* rep, uint32_t id, const uint8_t* d, uint32_t n, int64_t code, int as_text) {
    cbor_w w; cbor_w_init(&w, rb, PORT_MSG_MAX);
    kf_resp_begin(&w, id, 1);
    if (as_text) cbor_put_bytes(&w, d, n); else cbor_put_bytes(&w, d, n);
    kf_resp_end(&w, code);
    if (w.err) { reply_code(rep, id, FS_E_ARG); return; }
    (void)port_send(rep, 0, rb, (uint32_t)w.n);
}

/* Память-вложение <-> буфер ядра. Страницы - физические кадры, доступные ядру напрямую (так же работает proc.c). */
static uint32_t shm_bytes(kobj_t* a) { return a && a->type == KOBJ_SHM ? shm_pages(a) * 4096u : 0; }
static void shm_copy_out(kobj_t* a, const uint8_t* src, uint32_t n) {
    for (uint32_t i = 0, off = 0; off < n; i++, off += 4096) {
        uint32_t c = n - off < 4096 ? n - off : 4096;
        uint8_t* d = (uint8_t*)(uintptr_t)shm_frame(a, i);
        for (uint32_t k = 0; k < c; k++) d[k] = src[off + k];
    }
}
static void shm_copy_in(kobj_t* a, uint8_t* dst, uint32_t n) {
    for (uint32_t i = 0, off = 0; off < n; i++, off += 4096) {
        uint32_t c = n - off < 4096 ? n - off : 4096;
        const uint8_t* s = (const uint8_t*)(uintptr_t)shm_frame(a, i);
        for (uint32_t k = 0; k < c; k++) dst[off + k] = s[k];
    }
}

static int get_name(const kframe_t* f, char name[FSRV_NAME_MAX + 1]) {
    const uint8_t* p; uint32_t n;
    if (kf_get_text(f, "name", &p, &n) != KF_OK || n == 0 || n > FSRV_NAME_MAX) return 0;
    for (uint32_t i = 0; i < n; i++) name[i] = (char)p[i];
    name[n] = 0;
    return fsrv_name_ok(name);
}

static void serve(uint32_t pid, const kframe_t* f, kobj_t* att, kobj_t* rep, const char* prog, uint32_t caps) {
    unsigned op = 0;
    if (streq(f->op, "read")) op = FS_OP_READ;
    else if (streq(f->op, "write")) op = FS_OP_WRITE;
    else if (streq(f->op, "stat")) op = FS_OP_STAT;
    else if (streq(f->op, "list")) op = FS_OP_LIST;
    else if (streq(f->op, "remove")) op = FS_OP_REMOVE;
    if (!op) { reply_code(rep, f->id, FS_E_ARG); return; }
    if (!(caps & CAP_FS) || !fsrv_prog_ok(prog)) { reply_code(rep, f->id, FS_E_DENIED); return; }
    if (att && att->type != KOBJ_SHM) { reply_code(rep, f->id, FS_E_ARG); return; }
    (void)pid;
    fsrv_req_t* r = (fsrv_req_t*)kmalloc(sizeof *r);
    if (!r) { reply_code(rep, f->id, KFS_ERR_NOSPC); return; }
    __builtin_memset(r, 0, sizeof *r);
    r->op = op;
    for (unsigned i = 0; prog[i] && i < sizeof r->prog - 1; i++) r->prog[i] = prog[i];
    if (op != FS_OP_LIST && !get_name(f, r->name)) { kfree(r); reply_code(rep, f->id, FS_E_DENIED); return; }
    uint64_t off = 0, len = 0; uint32_t dlen = 0; const uint8_t* dp = 0;
    int have_len = kf_get_uint(f, "len", &len) == KF_OK;
    if (kf_get_uint(f, "off", &off) == KF_E_TYPE) { kfree(r); reply_code(rep, f->id, FS_E_ARG); return; }
    uint32_t shmb = shm_bytes(att);
    uint64_t cap = att ? shmb : INLINE_MAX;
    int64_t code = FS_E_ARG;
    r->off = off;
    switch (op) {
    case FS_OP_READ:
        if (!have_len || len == 0 || len > cap || len > fsrv_max_data()) break;
        r->len = len;
        break;
    case FS_OP_LIST:
        r->len = have_len ? len : 4096;
        if (r->len == 0 || r->len > INLINE_MAX) { r->len = 0; break; }
        break;
    case FS_OP_WRITE:
        if (att) { if (!have_len || len > shmb || len > fsrv_max_data()) break; r->len = len; }
        else {
            if (kf_get_bytes(f, "data", &dp, &dlen) != KF_OK || dlen > INLINE_MAX) break;
            r->len = dlen;
        }
        break;
    default: r->len = 0; break;                                 /* stat, remove */
    }
    int args_ok = (op == FS_OP_STAT || op == FS_OP_REMOVE) ? 1 : (r->len != 0 || (op == FS_OP_WRITE));
    if (!args_ok) { kfree(r); reply_code(rep, f->id, FS_E_ARG); return; }
    r->data = (uint8_t*)kmalloc(r->len ? r->len : 1);
    if (!r->data) { kfree(r); reply_code(rep, f->id, KFS_ERR_NOSPC); return; }
    if (op == FS_OP_WRITE) {
        if (att) shm_copy_in(att, r->data, (uint32_t)r->len);
        else for (uint32_t i = 0; i < dlen; i++) r->data[i] = dp[i];
    }
    fsrv_exec_locked(r);
    code = r->result;
    if ((op == FS_OP_READ || op == FS_OP_LIST) && code > 0) {
        if ((uint64_t)code > r->len) code = (int64_t)r->len;
        if (att) { shm_copy_out(att, r->data, (uint32_t)code); reply_code(rep, f->id, code); }
        else reply_data(rep, f->id, r->data, (uint32_t)code, code, op == FS_OP_LIST);
    } else reply_code(rep, f->id, code);
    kfree(r->data); kfree(r);
}

static void handle_one(void) {
    uint32_t from = 0; kobj_t* att = 0;
    int64_t n = port_recv_att(svc, mb, PORT_MSG_MAX, 0, &from, 0, &att);
    if (n < 0) return;
    char prog[16]; uint32_t caps = 0, dom = 0;
    if (n > PORT_MSG_MAX || !proc_info(from, prog, &caps, &dom)) { if (att) kobj_release(att); return; }     /* отправитель уже умер: отвечать некому */
    kframe_t f;
    int pe = kf_parse_req(mb, (uint32_t)n, &f);
    kobj_t* rep = bound(from);
    if (!pe && streq(f.op, "hello")) {
        if (!att || att->type != KOBJ_PORT) { if (att) kobj_release(att); if (rep) reply_code(rep, f.id, FS_E_ARG); return; }
        unsigned slot = NBIND;
        for (unsigned i = 0; i < NBIND; i++) if (bind[i].rep && bind[i].pid == from) { slot = i; break; }
        if (slot == NBIND) { unbind_dead(); for (unsigned i = 0; i < NBIND; i++) if (!bind[i].rep) { slot = i; break; } }
        if (slot == NBIND) { reply_code(att, f.id, FS_E_BUSY); kobj_release(att); return; }          /* привязки кончились: ответ - в присланный порт */
        if (bind[slot].rep) kobj_release(bind[slot].rep);
        bind[slot].pid = from; bind[slot].rep = att;
        reply_code(att, f.id, 0);
        return;
    }
    if (!rep) { if (att) kobj_release(att); return; }                                           /* без приветствия отвечать некуда */
    if (pe) { reply_code(rep, 0, SVC_CODE_FRAME - pe); if (att) kobj_release(att); return; }
    serve(from, &f, att, rep, prog, caps);
    if (att) kobj_release(att);
}

void fssvc_init(void) {
    if (svc) return;
    mb = (uint8_t*)kmalloc(PORT_MSG_MAX);
    rb = (uint8_t*)kmalloc(PORT_MSG_MAX);
    if (!mb || !rb || port_open_service("kira.fs", &svc) != 0) { svc = 0; uart_print("[KIRA FSSVC] port kira.fs not created\n"); return; }
    uart_print("[KIRA FSSVC] port kira.fs ready: CBOR requests, sender identified by the kernel\n");
}

void fssvc_poll(void) {
    static unsigned tick;
    if (!svc) return;
    for (int i = 0; i < 4 && port_queued(svc); i++) handle_one();
    if ((++tick & 0x3FF) == 0) unbind_dead();
}
