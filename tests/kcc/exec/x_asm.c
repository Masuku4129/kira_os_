#include "ktest.h"
int main(void) {
#if defined(__KIRA__) && defined(__aarch64__)
    __asm__ volatile ("dmb ish"); __asm__ volatile ("dmb ishst"); __asm__ volatile ("dmb ishld"); __asm__ volatile ("dmb sy");
    __asm__ volatile ("dsb ish"); __asm__ volatile ("dsb sy"); __asm__ volatile ("dsb st"); __asm__ volatile ("isb");
    __asm__ volatile ("yield"); __asm__ volatile ("nop"); __asm__ volatile ("sev");
    __asm__ volatile ("" ::: "memory");
    unsigned long t, f;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r"(t));
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f));
    pn(t != 0 && f != 0);
#else
    __asm__ volatile ("mfence"); __asm__ volatile ("lfence"); __asm__ volatile ("sfence"); __asm__ volatile ("pause"); __asm__ volatile ("nop");
    __asm__ volatile ("" ::: "memory");
#ifdef __KIRA__
    unsigned lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    pn(lo != 0 || hi != 0);
#else
    pn(1);
#endif
#endif
    long r = ksys(1, (unsigned long)"direct\n", 7, 0); pn(r);
    return 3;
}
