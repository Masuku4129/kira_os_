#include "ktest.h"
int main(void) {
    __asm__ volatile ("mfence"); __asm__ volatile ("lfence"); __asm__ volatile ("sfence"); __asm__ volatile ("pause"); __asm__ volatile ("nop");
    __asm__ volatile ("" ::: "memory");
#ifdef __KIRA__
    unsigned lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    pn(lo != 0 || hi != 0);
#else
    pn(1);
#endif
    long r = ksys(1, (unsigned long)"direct\n", 7, 0); pn(r);
    return 3;
}
