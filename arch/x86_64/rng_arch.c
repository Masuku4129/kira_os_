/* x86-64: счётчик тактов (RDTSC) и аппаратный генератор RDSEED/RDRAND (если CPUID его объявляет). */
#include <stdint.h>
#include "cpu.h"
#include "rng.h"

static int have = -1;                 /* -1 - не проверено; 0 - нет; 1 - RDRAND; 2 - RDSEED (и RDRAND) */
static void probe(void) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    have = (c >> 30) & 1;
    cpuid(0, &a, &b, &c, &d);
    if (have && a >= 7) { cpuid(7, &a, &b, &c, &d); if ((b >> 18) & 1) have = 2; }
}

uint64_t arch_cycles(void) { return rdtsc(); }

int arch_hw_random(uint64_t* v) {
    if (have < 0) probe();
    if (!have) return 0;
    for (int i = 0; i < 10; i++) {
        unsigned char ok;
        if (have == 2) __asm__ volatile ("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok));
        else __asm__ volatile ("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
        if (ok) return 1;
    }
    return 0;
}

const char* arch_hw_random_name(void) {
    if (have < 0) probe();
    return have == 2 ? "RDSEED" : have == 1 ? "RDRAND" : 0;
}
