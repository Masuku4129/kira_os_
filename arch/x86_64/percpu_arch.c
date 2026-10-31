#include "percpu.h"

#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

/* Данные ядра: GS_BASE = p (режим ядра), KERNEL_GS_BASE = 0 (значение процесса, его меняет swapgs). */
void percpu_install(percpu_t* p) {
    uint64_t v = (uint64_t)(uintptr_t)p;
    __asm__ volatile ("wrmsr" : : "c"(MSR_GS_BASE), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
    __asm__ volatile ("wrmsr" : : "c"(MSR_KERNEL_GS_BASE), "a"(0), "d"(0));
}
