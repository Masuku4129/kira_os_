#include <stdio.h>
#include <string.h>
#include "../linebuf.h"

static char emitted[LB_SIZE + 8];
static unsigned emitted_len;
static unsigned emits;

static void capture(const char *s, unsigned n, void *ctx) {
    (void)ctx;
    if (emitted_len + n > sizeof emitted) return;
    memcpy(emitted + emitted_len, s, n);
    emitted_len += n;
    emits++;
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void) {
    linebuf_t b = {0};
    char message[402];
    memset(message, 'K', 400);
    message[400] = '\n';
    message[401] = 0;

    for (unsigned i = 0; i < 400; i++) lb_putc(&b, message[i], capture, 0);
    CHECK(emits == 0);                    /* no partial flush before newline */
    CHECK(b.len == 400);
    lb_putc(&b, message[400], capture, 0);
    CHECK(emits == 1);                    /* entire long line emitted atomically */
    CHECK(emitted_len == 401);
    CHECK(memcmp(emitted, message, 401) == 0);
    CHECK(b.len == 0);

    emitted_len = emits = 0;
    for (unsigned i = 0; i < 120; i++) lb_putc(&b, 'x', capture, 0);
    lb_tick(&b, capture, 0);
    CHECK(emits == 0);
    lb_tick(&b, capture, 0);
    CHECK(emits == 1);                    /* incomplete prompt still flushes after its age */
    CHECK(emitted_len == 120);
    puts("linebuf: long lines remain atomic; idle prompt flush works");
    return 0;
}
