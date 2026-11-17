#include "kptr.h"
#include "uart.h"

extern char kernel_start[], kernel_end[];

static volatile int debug_on;                   // режим отладки: по умолчанию выключен

int kptr_debug(void) { return debug_on; }
void kptr_set_debug(int on) { debug_on = on ? 1 : 0; }

int kptr_in_image(uint64_t v) {
    return v >= (uint64_t)(uintptr_t)kernel_start && v < (uint64_t)(uintptr_t)kernel_end;
}

static unsigned put_hex(char* b, uint64_t v) {
    static const char d[] = "0123456789abcdef";
    unsigned n = 0, started = 0;
    b[n++] = '0'; b[n++] = 'x';
    for (int sh = 60; sh >= 0; sh -= 4) {
        unsigned x = (unsigned)((v >> sh) & 0xF);
        if (x || started || sh == 0) { b[n++] = d[x]; started = 1; }
    }
    b[n] = 0;
    return n;
}

unsigned kptr_format(uint64_t v, uint64_t image_lo, uint64_t image_hi, int debug, char* buf) {
    if (debug || v < image_lo || v >= image_hi) return put_hex(buf, v);
    buf[0] = 'K'; buf[1] = '+';
    return 2 + put_hex(buf + 2, v - image_lo);
}

void kptr_print(uint64_t v) {
    char b[24];
    kptr_format(v, (uint64_t)(uintptr_t)kernel_start, (uint64_t)(uintptr_t)kernel_end, debug_on, b);
    uart_print(b);
}
