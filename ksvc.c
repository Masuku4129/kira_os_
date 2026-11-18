#include "ksvc.h"
#include "proc.h"
#include "heap.h"
#include "uart.h"

static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static kobj_t* bound(ksvc_t* s, uint32_t pid) {
    for (unsigned i = 0; i < KSVC_BINDS; i++) if (s->bind[i].rep && s->bind[i].pid == pid) return s->bind[i].rep;
    return 0;
}
static void unbind_dead(ksvc_t* s) {
    for (unsigned i = 0; i < KSVC_BINDS; i++) {
        char nm[16]; uint32_t c, d;
        if (s->bind[i].rep && !proc_info(s->bind[i].pid, nm, &c, &d)) { kobj_release(s->bind[i].rep); s->bind[i].rep = 0; }
    }
}
static void send_code(ksvc_t* s, kobj_t* rep, uint32_t id, int64_t code) {
    uint32_t n = kf_resp_code(s->rb, PORT_MSG_MAX, id, code);
    if (n) (void)port_send(rep, 0, s->rb, n);          /* очередь клиента полна - ответ теряется: клиент сам не читает */
}

int ksvc_init(ksvc_t* s, const char* port_name) {
    if (s->port) return 0;
    s->mb = (uint8_t*)kmalloc(PORT_MSG_MAX);
    s->rb = (uint8_t*)kmalloc(PORT_MSG_MAX);
    if (!s->mb || !s->rb || port_open_service(port_name, &s->port) != 0) {
        kfree(s->mb); kfree(s->rb); s->mb = s->rb = 0; s->port = 0;
        uart_print("[KIRA SVC] port "); uart_print(port_name); uart_print(" not created\n");
        return -1;
    }
    return 0;
}

int ksvc_next(ksvc_t* s, ksvc_req_t* r) {
    if (!s->port) return 0;
    for (int guard = 0; guard < 8; guard++) {
        uint32_t from = 0; kobj_t* att = 0;
        int64_t n = port_recv_att(s->port, s->mb, PORT_MSG_MAX, 0, &from, 0, &att);
        if (n < 0) return 0;
        char prog[16]; uint32_t caps = 0, dom = 0;
        if (n > PORT_MSG_MAX || !proc_info(from, prog, &caps, &dom)) { if (att) kobj_release(att); continue; }   /* отправитель умер: отвечать некому */
        kframe_t f;
        int pe = kf_parse_req(s->mb, (uint32_t)n, &f);
        kobj_t* rep = bound(s, from);
        if (!pe && streq(f.op, "hello")) {
            if (!att || att->type != KOBJ_PORT) { if (att) kobj_release(att); if (rep) send_code(s, rep, f.id, KSVC_E_ARG); continue; }
            unsigned slot = KSVC_BINDS;
            for (unsigned i = 0; i < KSVC_BINDS; i++) if (s->bind[i].rep && s->bind[i].pid == from) { slot = i; break; }
            if (slot == KSVC_BINDS) { unbind_dead(s); for (unsigned i = 0; i < KSVC_BINDS; i++) if (!s->bind[i].rep) { slot = i; break; } }
            if (slot == KSVC_BINDS) { send_code(s, att, f.id, KSVC_E_BUSY); kobj_release(att); continue; }      /* привязки кончились: ответ - в присланный порт */
            if (s->bind[slot].rep) kobj_release(s->bind[slot].rep);
            s->bind[slot].pid = from; s->bind[slot].rep = att;
            send_code(s, att, f.id, 0);
            continue;
        }
        if (!rep) { if (att) kobj_release(att); continue; }                                          /* без приветствия отвечать некуда */
        if (pe) { send_code(s, rep, 0, KSVC_E_FRAME - pe); if (att) kobj_release(att); continue; }
        r->pid = from; r->caps = caps; r->dom = dom; r->f = f; r->att = att; r->rep = rep;
        for (unsigned i = 0; i < 16; i++) r->prog[i] = prog[i];
        return 1;
    }
    return 0;
}

void ksvc_done(ksvc_req_t* r) { if (r->att) { kobj_release(r->att); r->att = 0; } }

void ksvc_reply_code(ksvc_t* s, const ksvc_req_t* r, int64_t code) { send_code(s, r->rep, r->f.id, code); }

void ksvc_reply_body(ksvc_t* s, const ksvc_req_t* r, int64_t code, void (*body)(cbor_w* w, void* ctx), void* ctx) {
    cbor_w w; cbor_w_init(&w, s->rb, PORT_MSG_MAX);
    kf_resp_begin(&w, r->f.id, 1);
    body(&w, ctx);
    kf_resp_end(&w, code);
    if (w.err) { send_code(s, r->rep, r->f.id, KSVC_E_ARG); return; }
    (void)port_send(r->rep, 0, s->rb, (uint32_t)w.n);
}

void ksvc_housekeeping(ksvc_t* s) { if ((++s->tick & 0x3FF) == 0) unbind_dead(s); }
