/* Вещественные в variadic-вызовах: float -> double, va_arg(double), именованные параметры, больше восьми значений (стек), al = число xmm на x86-64. */
#include <stdarg.h>
#include <stdio.h>
static unsigned long long H = 1469598103934665603ULL;
static void mix(unsigned long long v) { H = (H ^ v) * 1099511628211ULL; H ^= H >> 29; }
static void md(double d) { union { double d; unsigned long long u; } x; x.d = d; mix(x.u); }
static double sum_d(int n, ...) {
    va_list ap; va_start(ap, n);
    double s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, double);
    va_end(ap);
    return s;
}
static double sum_mixed(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    double s = 0;
    for (; *fmt; fmt++) {
        if (*fmt == 'i') s += va_arg(ap, int);
        else if (*fmt == 'd') s += va_arg(ap, double);
        else if (*fmt == 'l') s += (double)va_arg(ap, long long);
    }
    va_end(ap);
    return s;
}
static double vnamed(double first, int n, ...) {
    va_list ap; va_start(ap, n);
    double s = first;
    for (int i = 0; i < n; i++) s = s * 2 + va_arg(ap, double);
    va_end(ap);
    return s;
}
int main(void) {
    md(sum_d(3, 1.0, 2.5, 3.75)); md(sum_d(10, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0)); md(sum_d(2, 0.1f, 0.2f)); md(sum_d(0));
    md(sum_mixed("idliddi", 1, 2.5, 3LL, 4, 5.5, 6.5, 7)); md(sum_mixed("ddddddddddd", 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0));
    md(vnamed(1.5, 3, 2.0, 3.0, 4.0)); md(vnamed(0.5, 0));
    float f = 1.25f; md(sum_d(2, f, f * 2)); md(sum_d(1, (float)0.1));
    md(sum_mixed("iiiiiiiidddddddddd", 1, 2, 3, 4, 5, 6, 7, 8, 0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5));
    char buf[64]; snprintf(buf, sizeof buf, "%d|%s|%c", 12, "ab", 'q'); puts(buf);
    printf("%llx\n", H);
    return (int)(H & 0x7f);
}
