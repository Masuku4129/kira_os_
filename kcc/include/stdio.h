/* kcc libc-lite: stdio.h поверх k_write. Честно: printf/snprintf нет - kcc пока не умеет определять функции с переменным
   числом аргументов (va_list). Вместо них - put_dec/put_udec/put_hex. Вывод идёт в журнал ядра построчно ("[USER pid N] ..."). */
#ifndef KCC_STDIO_H
#define KCC_STDIO_H
#include <stddef.h>
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
#endif
