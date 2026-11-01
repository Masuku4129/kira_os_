/* Программа для проверки kcc на Кире: функции, структуры, строки, цикл, вывод через kira.h / libc-lite. */
#include <kira.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct item { const char *name; int qty; int price; };

static struct item items[] = { { "bolt", 12, 5 }, { "nut", 30, 2 }, { "gear", 3, 40 } };

static int total(const struct item *it, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) s += it[i].qty * it[i].price;
    return s;
}

static int by_qty(const void *a, const void *b) {
    return ((const struct item *)a)->qty - ((const struct item *)b)->qty;
}

static unsigned fib(unsigned n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static void reverse(char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n / 2; i++) { char t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; }
}

int main(void) {
    puts("native: hello from kcc on Kira");
    fputs_out("native: total=");
    put_dec(total(items, 3));
    putchar('\n');
    fputs_out("native: fib20=");
    put_udec(fib(20));
    putchar('\n');
    qsort(items, 3, sizeof items[0], by_qty);
    fputs_out("native: sorted");
    for (int i = 0; i < 3; i++) { putchar(' '); fputs_out(items[i].name); }
    putchar('\n');
    char *buf = malloc(64);
    if (!buf) { puts("native: malloc failed"); return 1; }
    strcpy(buf, "kira");
    strcat(buf, "-os");
    reverse(buf);
    fputs_out("native: reversed=");
    puts(buf);
    free(buf);
    fputs_out("native: hex=");
    put_hex(0xC0FFEEul);
    putchar('\n');
    u64 t0 = k_ticks();
    k_sleep(1);
    puts(k_ticks() >= t0 ? "native: clock ok" : "native: clock BAD");
    puts("native: done");
    return 0;
}
