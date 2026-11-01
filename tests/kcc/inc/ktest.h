/* Вывод для exec-тестов: в Кире - системный вызов K_WRITE через встроенный asm, на хосте (эталон clang) - write(1). */
#ifndef KTEST_H
#define KTEST_H
#ifdef __KIRA__
static long ksys(unsigned long n, unsigned long a, unsigned long b, unsigned long c) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r), "+D"(a), "+S"(b), "+d"(c) : "a"(n) : "rcx", "r11", "r8", "r9", "r10", "memory");
    return r;
}
#else
#include <unistd.h>
static long ksys(unsigned long n, unsigned long a, unsigned long b, unsigned long c) {
    (void)c;
    if (n == 1) return write(1, (const void *)a, b);
    return 0;
}
#endif
static unsigned long kslen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void ps(const char *s) { ksys(1, (unsigned long)s, kslen(s), 0); }
static void pd(long v) {
    char b[24]; int i = 24; unsigned long u = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) b[--i] = '-';
    ksys(1, (unsigned long)(b + i), (unsigned long)(24 - i), 0);
}
static void pn(long v) { pd(v); ps("\n"); }
static void px(unsigned long v) {
    char b[20]; int i = 20;
    do { b[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    ksys(1, (unsigned long)(b + i), (unsigned long)(20 - i), 0);
}
#endif
