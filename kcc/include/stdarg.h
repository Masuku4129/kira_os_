/* kcc libc-lite: переменное число аргументов. va_list - тип, который выбирает kcc под цель (x86-64 SysV или AAPCS64). */
#ifndef KCC_STDARG_H
#define KCC_STDARG_H
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) __builtin_va_end(ap)
#define va_copy(dst, src) __builtin_va_copy(dst, src)
#endif
