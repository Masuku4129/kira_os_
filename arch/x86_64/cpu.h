#ifndef CPU_H
#define CPU_H

#include <stdint.h>

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpuid(uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static inline uint64_t cpu_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpu_wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

// Состояние FPU/SSE (FXSAVE-область: 512 байт, выравнивание 16 обязательно, иначе #GP).
// В хост-тесте по умолчанию заглушки; KIRA_HOST_REAL_FPU включает настоящие команды.
#if defined(KIRA_HOST_TEST) && !defined(KIRA_HOST_REAL_FPU)
static inline void fpu_save(void* area) { (void)area; }
static inline void fpu_restore(const void* area) { (void)area; }
#else
static inline void fpu_save(void* area) {
    __asm__ volatile ("fxsave (%0)" : : "r"(area) : "memory");
}
static inline void fpu_restore(const void* area) {
    __asm__ volatile ("fxrstor (%0)" : : "r"(area) : "memory");
}
#endif

// Критические секции: запрет прерываний с сохранением прежнего состояния IF.
// Безопасно вкладывать друг в друга.
#ifdef KIRA_HOST_TEST
static inline uint64_t irq_save(void) { return 0; }
static inline void irq_restore(uint64_t flags) { (void)flags; }
#else
static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tcli\n\tpopq %0" : "=r"(flags) : : "memory");
    return flags;
}
static inline void irq_restore(uint64_t flags) {
    if (flags & (1ULL << 9)) {
        __asm__ volatile ("sti" : : : "memory");
    }
}
#endif

#ifndef KIRA_HOST_TEST
static inline void cpu_halt(void) { __asm__ volatile ("hlt"); }
static inline void cpu_irq_off(void) { __asm__ volatile ("cli" ::: "memory"); }
static inline void cpu_irq_on(void) { __asm__ volatile ("sti" ::: "memory"); }
#endif

#endif
