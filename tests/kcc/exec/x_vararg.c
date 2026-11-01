#include "ktest.h"
#include <stdarg.h>
/* функции с переменным числом аргументов: va_start/va_arg/va_end/va_copy, более шести аргументов, va_list в другую функцию */
static long sumn(int n, ...) {
    va_list ap; va_start(ap, n);
    long s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, long);
    va_end(ap);
    return s;
}
static int mixv(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int acc = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p == 'i') acc += va_arg(ap, int);
        else if (*p == 'l') acc += (int)va_arg(ap, long);
        else if (*p == 'c') acc += va_arg(ap, int);
        else if (*p == 'u') acc += (int)va_arg(ap, unsigned);
        else if (*p == 's') { const char *s = va_arg(ap, const char *); while (*s++) acc++; }
        else if (*p == 'p') { int *q = va_arg(ap, int *); acc += *q; }
    }
    va_end(ap);
    return acc;
}
/* мини-форматтер поверх va_list, переданного по значению/указателю (по ABI цели) */
static int fmt_u(char *o, unsigned long v, int base, int neg) {
    char t[24]; int n = 0, k = 0;
    do { t[n++] = "0123456789abcdef"[v % (unsigned)base]; v /= (unsigned)base; } while (v);
    if (neg) o[k++] = '-';
    while (n) o[k++] = t[--n];
    return k;
}
static int vfmt(char *o, const char *f, va_list ap) {
    int k = 0;
    for (; *f; f++) {
        if (*f != '%') { o[k++] = *f; continue; }
        f++;
        int lng = 0;
        if (*f == 'l') { lng = 1; f++; }
        if (*f == 'd') { long v = lng ? va_arg(ap, long) : va_arg(ap, int); k += fmt_u(o + k, v < 0 ? 0UL - (unsigned long)v : (unsigned long)v, 10, v < 0); }
        else if (*f == 'u') { unsigned long v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned); k += fmt_u(o + k, v, 10, 0); }
        else if (*f == 'x') { unsigned long v = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned); k += fmt_u(o + k, v, 16, 0); }
        else if (*f == 'c') o[k++] = (char)va_arg(ap, int);
        else if (*f == 's') { const char *s = va_arg(ap, const char *); while (*s) o[k++] = *s++; }
        else o[k++] = *f;
    }
    o[k] = 0;
    return k;
}
static int my_sprintf(char *o, const char *f, ...) {
    va_list ap; va_start(ap, f);
    int n = vfmt(o, f, ap);
    va_end(ap);
    return n;
}
static long twice(int n, ...) {          /* va_copy: сумма читается дважды */
    va_list a, b; va_start(a, n); va_copy(b, a);
    long s = 0;
    for (int i = 0; i < n; i++) s += va_arg(a, int);
    for (int i = 0; i < n; i++) s += va_arg(b, int) * 100;
    va_end(a); va_end(b);
    return s;
}
static long ptrs(const char *first, ...) {
    va_list ap; va_start(ap, first);
    long n = 0; const char *p = first;
    while (p) { n += (long)(*p) ; p = va_arg(ap, const char *); }
    va_end(ap);
    return n;
}
static long after_fixed(long a, long b, long c, long d, long e, long f, long g, ...) {   /* именованные аргументы уже заняли все регистры и стек */
    va_list ap; va_start(ap, g);
    long s = a + b + c + d + e + f + g;
    s += va_arg(ap, long) * 1000; s += va_arg(ap, int) * 10000L;
    va_end(ap);
    return s;
}
int main(void) {
    pn(sumn(0)); pn(sumn(1, 5L)); pn(sumn(3, 1L, 2L, 3L)); pn(sumn(10, 1L, 2L, 3L, 4L, 5L, 6L, 7L, 8L, 9L, 10L));
    int q = 100;
    pn(mixv("iluc", 1, 2L, 3u, 'a')); pn(mixv("sp", "hello", &q)); pn(mixv("iiiiiiiiii", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10)); pn(mixv(""));
    char buf[128];
    int n = my_sprintf(buf, "x=%d u=%u h=%x c=%c s=%s l=%ld %%", -42, 4000000000u, 255, 'Q', "str", -5000000000L);
    pn(n); ps(buf); ps("\n");
    n = my_sprintf(buf, "%d %d %d %d %d %d %d %d", 1, 2, 3, 4, 5, 6, 7, 8); ps(buf); ps("\n");
    pn(twice(4, 1, 2, 3, 4));
    pn(ptrs("A", "B", "C", (const char *)0));
    pn(after_fixed(1, 2, 3, 4, 5, 6, 7, 8L, 9));
    return 0;
}
