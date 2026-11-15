#include "gfxctl.h"
#include "gfxsrv.h"
#include "ksvc.h"
#include "caps.h"
#include "fb.h"
#include "uart.h"

static ksvc_t svc;
static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void put_screen(cbor_w* w, void* ctx) {
    const fb_info_t* fi = fb_get_info();
    (void)ctx;
    cbor_put_map(w, 2);
    cbor_put_cstr(w, "h"); cbor_put_uint(w, fi && fi->height ? fi->height : 768);
    cbor_put_cstr(w, "w"); cbor_put_uint(w, fi && fi->width ? fi->width : 1024);
}
typedef struct { uint32_t w, h; int focus; } wininfo_t;
static void put_win(cbor_w* w, void* ctx) {
    wininfo_t* i = (wininfo_t*)ctx;
    cbor_put_map(w, 3);
    cbor_put_cstr(w, "h"); cbor_put_uint(w, i->h);
    cbor_put_cstr(w, "w"); cbor_put_uint(w, i->w);
    cbor_put_cstr(w, "focus"); cbor_put_bool(w, i->focus);
}
typedef struct { gfx_event_t e[16]; uint32_t n; } evs_t;
static void put_events(cbor_w* w, void* ctx) {
    evs_t* v = (evs_t*)ctx;
    cbor_put_array(w, v->n);
    for (uint32_t i = 0; i < v->n; i++) {
        cbor_put_array(w, 4);
        cbor_put_uint(w, v->e[i].type); cbor_put_uint(w, v->e[i].a); cbor_put_uint(w, v->e[i].b); cbor_put_uint(w, v->e[i].c);
    }
}

static void serve(const ksvc_req_t* q) {
    if (!(q->caps & CAP_GFX)) { ksvc_reply_code(&svc, q, -100); return; }
    if (streq(q->f.op, "info")) { ksvc_reply_body(&svc, q, 0, put_screen, 0); return; }
    if (streq(q->f.op, "win")) {
        wininfo_t i;
        if (gfx_win_info(q->pid, &i.w, &i.h, &i.focus) != 0) { ksvc_reply_code(&svc, q, -2); return; }
        ksvc_reply_body(&svc, q, 0, put_win, &i);
        return;
    }
    if (streq(q->f.op, "events")) {
        uint64_t max = 8;
        int r = kf_get_uint(&q->f, "max", &max);
        if (r != KF_OK && r != KF_ABSENT) { ksvc_reply_code(&svc, q, KSVC_E_ARG); return; }
        if (max == 0 || max > 16) { ksvc_reply_code(&svc, q, KSVC_E_ARG); return; }
        evs_t v; v.n = 0;
        while (v.n < max) {
            int t = gfx_event_take(q->pid, &v.e[v.n]);
            if (t < 0) { ksvc_reply_code(&svc, q, -2); return; }
            if (t == 0) break;
            v.n++;
        }
        ksvc_reply_body(&svc, q, (int64_t)v.n, put_events, &v);
        return;
    }
    ksvc_reply_code(&svc, q, KSVC_E_ARG);
}

void gfxctl_init(void) {
    if (ksvc_init(&svc, "kira.gfx") == 0) uart_print("[KIRA GFXCTL] port kira.gfx ready: window and event queries over CBOR\n");
}
void gfxctl_poll(void) {
    ksvc_req_t q;
    for (int i = 0; i < 4 && ksvc_next(&svc, &q); i++) { serve(&q); ksvc_done(&q); }
    ksvc_housekeeping(&svc);
}
