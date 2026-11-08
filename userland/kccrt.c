/* libc для kcc на Кире: строки, qsort, vsnprintf, setjmp/longjmp (x86-64 и ARM64). mem* - в userland/libc_min.c. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "kccrt/string.h"
#include "kccrt/stdlib.h"
#include "kccrt/stdio.h"
#include "kccrt/setjmp.h"

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
    }
    return 0;
}
char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return 0;
    }
}
char *strrchr(const char *s, int c) {
    const char *r = 0;
    for (;; s++) {
        if (*s == (char)c) r = s;
        if (!*s) return (char *)r;
    }
}
char *strstr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++) if (*h == *n && !strncmp(h, n, nl)) return (char *)h;
    return 0;
}

/* Пирамидальная сортировка: без рекурсии и без дополнительной памяти, O(n log n) в худшем случае. */
static void swapb(unsigned char *a, unsigned char *b, size_t n) {
    while (n--) { unsigned char t = *a; *a++ = *b; *b++ = t; }
}
static void sift(unsigned char *base, size_t size, size_t i, size_t n, int (*cmp)(const void *, const void *)) {
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= n) return;
        if (c + 1 < n && cmp(base + c * size, base + (c + 1) * size) < 0) c++;
        if (cmp(base + i * size, base + c * size) >= 0) return;
        swapb(base + i * size, base + c * size, size);
        i = c;
    }
}
void qsort(void *vb, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    unsigned char *base = vb;
    if (n < 2 || !size) return;
    for (size_t i = n / 2; i-- > 0;) sift(base, size, i, n, cmp);
    for (size_t e = n - 1; e > 0; e--) { swapb(base, base + e * size, size); sift(base, size, 0, e, cmp); }
}

/* vsnprintf: флаги - + пробел 0 #, ширина и точность (в том числе *), длины hh h l ll z j t, преобразования d i u x X o c s p %.
   Возвращает длину полного результата; в буфер пишет не больше cap-1 знаков и завершающий 0. */
typedef struct { char *b; size_t cap, n; } outbuf;
static void rt_put(outbuf *o, char c) { if (o->n + 1 < o->cap) o->b[o->n] = c; o->n++; }
static void pad(outbuf *o, char c, long n) { while (n-- > 0) rt_put(o, c); }

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    outbuf o = { buf, cap, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { rt_put(&o, *fmt); continue; }
        fmt++;
        int left = 0, plus = 0, space = 0, zero = 0, alt = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1; else if (*fmt == '+') plus = 1; else if (*fmt == ' ') space = 1;
            else if (*fmt == '0') zero = 1; else if (*fmt == '#') alt = 1; else break;
        }
        long width = 0, prec = -1;
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; if (prec < 0) prec = -1; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        int lng = 0;                                   /* 0 int, 1 long, 2 long long/size_t, -1 short, -2 char */
        for (;; fmt++) {
            if (*fmt == 'l') lng = lng >= 1 ? 2 : 1;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') lng = 2;
            else if (*fmt == 'h') lng = lng <= -1 ? -2 : -1;
            else break;
        }
        char c = *fmt;
        if (!c) break;
        if (c == '%') { rt_put(&o, '%'); continue; }
        char tmp[72]; const char *s = tmp; long len = 0; const char *prefix = ""; int numeric = 0;
        if (c == 'c') { tmp[0] = (char)va_arg(ap, int); len = 1; }
        else if (c == 's') {
            s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while ((prec < 0 || len < prec) && s[len]) len++;
        } else if (c == 'd' || c == 'i') {
            long long v = lng >= 2 ? va_arg(ap, long long) : lng == 1 ? va_arg(ap, long) : va_arg(ap, int);
            if (lng == -1) v = (short)v; else if (lng == -2) v = (signed char)v;
            unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
            prefix = v < 0 ? "-" : plus ? "+" : space ? " " : "";
            char r[24]; int k = 0;
            do { r[k++] = (char)('0' + u % 10); u /= 10; } while (u);
            while (k) tmp[len++] = r[--k];
            numeric = 1;
        } else if (c == 'u' || c == 'x' || c == 'X' || c == 'o' || c == 'p') {
            unsigned long long u;
            if (c == 'p') { u = (unsigned long long)(uintptr_t)va_arg(ap, void *); alt = 1; c = 'x'; }
            else {
                u = lng >= 2 ? va_arg(ap, unsigned long long) : lng == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
                if (lng == -1) u = (unsigned short)u; else if (lng == -2) u = (unsigned char)u;
            }
            unsigned base = c == 'u' ? 10 : c == 'o' ? 8 : 16;
            const char *dg = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            int nonzero = u != 0;
            char r[24]; int k = 0;
            do { r[k++] = dg[u % base]; u /= base; } while (u);
            while (k) tmp[len++] = r[--k];
            if (alt && nonzero && c != 'u') prefix = c == 'X' ? "0X" : c == 'x' ? "0x" : "0";
            numeric = 1;
        } else { rt_put(&o, '%'); rt_put(&o, c); continue; }   /* неизвестное преобразование выводится как есть */
        long zeros = 0, plen = (long)strlen(prefix);
        if (numeric) {
            if (prec >= 0) { zero = 0; if (prec > len) zeros = prec - len; if (prec == 0 && len == 1 && tmp[0] == '0') len = 0; }
            else if (zero && !left && width > len + plen) zeros = width - len - plen;
        }
        long total = len + zeros + plen;
        if (!left) pad(&o, ' ', width - total);
        for (const char *p = prefix; *p; p++) rt_put(&o, *p);
        pad(&o, '0', zeros);
        for (long i = 0; i < len; i++) rt_put(&o, s[i]);
        if (left) pad(&o, ' ', width - total);
    }
    if (cap) o.b[o.n < cap ? o.n : cap - 1] = 0;
    return (int)o.n;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

/* setjmp/longjmp: сохраняются callee-saved регистры, стек и адрес возврата. setjmp возвращает 0, longjmp(env, v) - v (0 -> 1). */
#if defined(__KIRA__) && defined(__x86_64__)
/* kcc (самосборка): setjmp/longjmp - встроенные функции компилятора (kcc/gen_x86.c), текст ассемблера не нужен. */
#elif defined(__aarch64__)
/* x19-x30 и sp (callee-saved целочисленные; вещественных регистров код kcc не использует: сборка с -mgeneral-regs-only) */
__asm__(".text\n.global setjmp\nsetjmp:\n"
        " stp x19, x20, [x0, #0]\n stp x21, x22, [x0, #16]\n stp x23, x24, [x0, #32]\n stp x25, x26, [x0, #48]\n stp x27, x28, [x0, #64]\n"
        " stp x29, x30, [x0, #80]\n mov x9, sp\n str x9, [x0, #96]\n mov w0, #0\n ret\n"
        ".global longjmp\nlongjmp:\n"
        " ldp x19, x20, [x0, #0]\n ldp x21, x22, [x0, #16]\n ldp x23, x24, [x0, #32]\n ldp x25, x26, [x0, #48]\n ldp x27, x28, [x0, #64]\n"
        " ldp x29, x30, [x0, #80]\n ldr x9, [x0, #96]\n mov sp, x9\n cmp w1, #0\n csinc w0, w1, wzr, ne\n ret\n");
#else
__asm__(".text\n.global setjmp\nsetjmp:\n"
        " mov %rbx, 0(%rdi)\n mov %rbp, 8(%rdi)\n mov %r12, 16(%rdi)\n mov %r13, 24(%rdi)\n mov %r14, 32(%rdi)\n mov %r15, 40(%rdi)\n"
        " lea 8(%rsp), %rdx\n mov %rdx, 48(%rdi)\n mov (%rsp), %rdx\n mov %rdx, 56(%rdi)\n xor %eax, %eax\n ret\n"
        ".global longjmp\nlongjmp:\n"
        " mov %esi, %eax\n test %eax, %eax\n jnz 1f\n inc %eax\n1:\n"
        " mov 0(%rdi), %rbx\n mov 8(%rdi), %rbp\n mov 16(%rdi), %r12\n mov 24(%rdi), %r13\n mov 32(%rdi), %r14\n mov 40(%rdi), %r15\n"
        " mov 48(%rdi), %rsp\n jmp *56(%rdi)\n");
#endif
