#ifndef CRASHDUMP_H
#define CRASHDUMP_H

#include <stdint.h>

// Раскладка кадра исключения (то, что кладёт interrupt_common_stub), индексы в uint64_t:
// 0=r15 1=r14 2=r13 3=r12 4=r11 5=r10 6=r9 7=r8 8=rbp 9=rdi 10=rsi 11=rdx 12=rcx 13=rbx 14=rax
// 15=вектор 16=код ошибки 17=rip 18=cs 19=rflags 20=rsp 21=ss
#define CRASH_FRAME_QWORDS 22

typedef void (*crash_out_fn)(const char*);

typedef struct { uint64_t cr0, cr2, cr3, cr4; } crash_regs_t;

// Печатает регистры и (если stack != NULL) восемь слов на вершине стека. Только вывод через out,
// без зависимостей от ядра, поэтому проверяется хост-тестом.
void crashdump_print(const uint64_t* frame, const crash_regs_t* cr, const uint64_t* stack, crash_out_fn out);

#endif
