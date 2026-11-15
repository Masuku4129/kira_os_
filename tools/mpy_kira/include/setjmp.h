/* setjmp/longjmp для wasm32 на Кире: вызовы интерпретатора userland/wasm.c (модуль "kira"), без расширения WASM «исключения».
   jmp_buf хранит только номер снимка и поколение (8 байт): состояние выполнения хранит интерпретатор (см. userland/wasm.c). */
#ifndef KIRA_SETJMP_H
#define KIRA_SETJMP_H
typedef struct { unsigned idx, gen; } jmp_buf[1];
__attribute__((import_module("kira"), import_name("setjmp"), returns_twice)) int setjmp(jmp_buf env);
__attribute__((import_module("kira"), import_name("longjmp"), noreturn)) void longjmp(jmp_buf env, int val);
#endif
