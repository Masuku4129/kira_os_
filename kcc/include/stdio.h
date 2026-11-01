/* kcc libc-lite: stdio.h поверх k_write. printf-семейство: %d %i %u %x %X %o %c %s %p %% с флагами '-' и '0', шириной (число или *),
   точностью (для строк и целых), длинами hh h l ll z. Вещественных чисел нет (kcc их не поддерживает). Одна печать printf - до 511 байт
   (длиннее обрезается, возвращается полная длина); строка выходит в журнал ядра по знаку конца строки ("[USER pid N] ..."). */
#ifndef KCC_STDIO_H
#define KCC_STDIO_H
#include <stddef.h>
#include <stdarg.h>
#include <kira.h>
static int putchar(int c) { char b = (char)c; k_write(&b, 1); return (unsigned char)c; }
static int puts(const char *s) { k_write(s, k_strlen(s)); putchar('\n'); return 0; }
static int fputs_out(const char *s) { k_write(s, k_strlen(s)); return 0; }          /* без перевода строки */
static void put_udec(unsigned long v) {
    char b[24]; int i = 24;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    k_write(b + i, (u64)(24 - i));
}
static void put_dec(long v) {
    if (v < 0) { putchar('-'); put_udec(0UL - (unsigned long)v); }
    else put_udec((unsigned long)v);
}
static void put_hex(unsigned long v) {
    char b[20]; int i = 20;
    do { b[--i] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    k_write(b + i, (u64)(20 - i));
}

static int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    size_t k = 0;
#define KCC_OUT(ch) do { if (k + 1 < n) buf[k] = (char)(ch); k++; } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') { KCC_OUT(*fmt); continue; }
        fmt++;
        int left = 0, zero = 0, plus = 0, spc = 0, alt = 0, width = 0, prec = -1, lng = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') spc = 1;
            else if (*fmt == '#') alt = 1;
            else break;
        }
        if (*fmt == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            fmt++; prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        for (;; fmt++) {
            if (*fmt == 'l') lng = lng == 1 ? 2 : 1;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') lng = 1;
            else if (*fmt == 'h') lng = lng < 0 ? -2 : -1;
            else break;
        }
        char c = *fmt;
        if (!c) break;
        char tmp[40];
        const char *str = tmp;
        int len = 0;
        char sg = 0;
        const char *pre = "";
        if (c == '%') { KCC_OUT('%'); continue; }
        if (c == 'c') { tmp[0] = (char)va_arg(ap, int); len = 1; }
        else if (c == 's') {
            str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            while (str[len] && (prec < 0 || len < prec)) len++;
        } else if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X' || c == 'o' || c == 'p') {
            unsigned long long v;
            int base = c == 'o' ? 8 : (c == 'x' || c == 'X' || c == 'p') ? 16 : 10;
            if (c == 'd' || c == 'i') {
                long long sv = lng >= 2 ? va_arg(ap, long long) : lng == 1 ? va_arg(ap, long) : va_arg(ap, int);
                if (lng == -1) sv = (short)sv; else if (lng == -2) sv = (signed char)sv;
                if (sv < 0) { sg = '-'; v = 0ULL - (unsigned long long)sv; } else { v = (unsigned long long)sv; sg = plus ? '+' : spc ? ' ' : 0; }
            } else if (c == 'p') { v = (unsigned long long)va_arg(ap, void *); pre = "0x"; }
            else {
                v = lng >= 2 ? va_arg(ap, unsigned long long) : lng == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
                if (lng == -1) v = (unsigned short)v; else if (lng == -2) v = (unsigned char)v;
            }
            if (alt && v != 0 && (c == 'x' || c == 'X')) pre = c == 'X' ? "0X" : "0x";
            const char *dg = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char rev[24]; int nr = 0;
            while (v) { rev[nr++] = dg[v % (unsigned)base]; v /= (unsigned)base; }
            int nz = prec > nr ? prec - nr : 0;
            if (prec < 0 && nr == 0) nz = 1;
            if (prec == 0 && nr == 0) nz = 0;
            if (alt && c == 'o' && nz == 0 && (nr == 0 || rev[nr - 1] != '0')) nz = 1;
            for (int i = 0; i < nz; i++) tmp[len++] = '0';
            while (nr) tmp[len++] = rev[--nr];
            if (prec >= 0) zero = 0;
        } else { KCC_OUT('%'); KCC_OUT(c); continue; }
        int plen = (int)k_strlen(pre) + (sg != 0);
        int pad = width > len + plen ? width - len - plen : 0;
        if (!left && !zero) for (int i = 0; i < pad; i++) KCC_OUT(' ');
        if (sg) KCC_OUT(sg);
        for (const char *p = pre; *p; p++) KCC_OUT(*p);
        if (!left && zero) for (int i = 0; i < pad; i++) KCC_OUT('0');
        for (int i = 0; i < len; i++) KCC_OUT(str[i]);
        if (left) for (int i = 0; i < pad; i++) KCC_OUT(' ');
    }
    if (n) buf[k < n ? k : n - 1] = 0;
    return (int)k;
#undef KCC_OUT
}
static int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
static int printf(const char *fmt, ...) {
    char b[512];
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    k_write(b, (u64)(r < 511 ? r : 511));
    return r;
}
#endif
