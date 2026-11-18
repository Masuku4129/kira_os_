#include "fssvc.h"
#include "fsrv.h"
#include "kfs.h"
#include "kobj.h"
#include "ksvc.h"
#include "caps.h"
#include "heap.h"
#include "uart.h"
#include "userland/kframe.h"

#define INLINE_MAX (48u * 1024)          /* данные внутри сообщения: остальное - через вложенную память */

static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static ksvc_t svc;

static void reply_code(const ksvc_req_t* q, int64_t code) { ksvc_reply_code(&svc, q, code); }
typedef struct { const uint8_t* d; uint32_t n; } blob_t;
static void put_blob(cbor_w* w, void* ctx) { blob_t* b = (blob_t*)ctx; cbor_put_bytes(w, b->d, b->n); }

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

static void serve(const ksvc_req_t* q) {
    const kframe_t* f = &q->f; kobj_t* att = q->att; const char* prog = q->prog; uint32_t caps = q->caps;
    unsigned op = 0;
    if (streq(f->op, "read")) op = FS_OP_READ;
    else if (streq(f->op, "write")) op = FS_OP_WRITE;
    else if (streq(f->op, "stat")) op = FS_OP_STAT;
    else if (streq(f->op, "list")) op = FS_OP_LIST;
    else if (streq(f->op, "remove")) op = FS_OP_REMOVE;
    if (!op) { reply_code(q, FS_E_ARG); return; }
    if (!(caps & CAP_FS) || !fsrv_prog_ok(prog)) { reply_code(q, FS_E_DENIED); return; }
    if (att && att->type != KOBJ_SHM) { reply_code(q, FS_E_ARG); return; }
    fsrv_req_t* r = (fsrv_req_t*)kmalloc(sizeof *r);
    if (!r) { reply_code(q, KFS_ERR_NOSPC); return; }
    __builtin_memset(r, 0, sizeof *r);
    r->op = op;
    for (unsigned i = 0; prog[i] && i < sizeof r->prog - 1; i++) r->prog[i] = prog[i];
    if (op != FS_OP_LIST && !get_name(f, r->name)) { kfree(r); reply_code(q, FS_E_DENIED); return; }
    uint64_t off = 0, len = 0; uint32_t dlen = 0; const uint8_t* dp = 0;
    int have_len = kf_get_uint(f, "len", &len) == KF_OK;
    if (kf_get_uint(f, "off", &off) == KF_E_TYPE) { kfree(r); reply_code(q, FS_E_ARG); return; }
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
    if (!args_ok) { kfree(r); reply_code(q, FS_E_ARG); return; }
    r->data = (uint8_t*)kmalloc(r->len ? r->len : 1);
    if (!r->data) { kfree(r); reply_code(q, KFS_ERR_NOSPC); return; }
    if (op == FS_OP_WRITE) {
        if (att) shm_copy_in(att, r->data, (uint32_t)r->len);
        else for (uint32_t i = 0; i < dlen; i++) r->data[i] = dp[i];
    }
    fsrv_exec_locked(r);
    code = r->result;
    if ((op == FS_OP_READ || op == FS_OP_LIST) && code > 0) {
        if ((uint64_t)code > r->len) code = (int64_t)r->len;
        if (att) { shm_copy_out(att, r->data, (uint32_t)code); reply_code(q, code); }
        else { blob_t b = { r->data, (uint32_t)code }; ksvc_reply_body(&svc, q, code, put_blob, &b); }
    } else reply_code(q, code);
    kfree(r->data); kfree(r);
}

void fssvc_init(void) {
    if (ksvc_init(&svc, "kira.fs") == 0) uart_print("[KIRA FSSVC] port kira.fs ready: CBOR requests, sender identified by the kernel\n");
}

void fssvc_poll(void) {
    ksvc_req_t q;
    for (int i = 0; i < 4 && ksvc_next(&svc, &q); i++) { serve(&q); ksvc_done(&q); }
    ksvc_housekeeping(&svc);
}
