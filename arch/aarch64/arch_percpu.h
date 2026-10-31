#ifndef ARCH_PERCPU_H
#define ARCH_PERCPU_H
/* ARM64: адрес данных ядра - в TPIDR_EL1 (программам он не виден). */
static inline percpu_t* this_cpu(void) {
    percpu_t* p;
    __asm__ volatile ("mrs %0, tpidr_el1" : "=r"(p));
    return p;
}
#endif
