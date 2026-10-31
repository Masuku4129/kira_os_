#ifndef ARCH_PERCPU_H
#define ARCH_PERCPU_H
/* x86-64: адрес данных ядра - в IA32_GS_BASE, поле self (смещение 0) читается одной командой. */
static inline percpu_t* this_cpu(void) {
    percpu_t* p;
    __asm__ volatile ("mov %%gs:0, %0" : "=r"(p));
    return p;
}
#endif
