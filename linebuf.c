#include "linebuf.h"

void lb_flush(linebuf_t* b, lb_emit_fn emit, void* ctx) {
    if (b->len) emit(b->buf, b->len, ctx);
    b->len = 0;
    b->age = 0;
}

void lb_putc(linebuf_t* b, char c, lb_emit_fn emit, void* ctx) {
    b->buf[b->len++] = c;
    b->age = 0;
    if (c == '\n' || b->len == LB_SIZE) lb_flush(b, emit, ctx);
}

void lb_tick(linebuf_t* b, lb_emit_fn emit, void* ctx) {
    if (!b->len) { b->age = 0; return; }
    if (++b->age >= LB_FLUSH_AGE) lb_flush(b, emit, ctx);
}
