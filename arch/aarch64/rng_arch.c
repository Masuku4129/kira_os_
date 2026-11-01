/* ARM64: счётчик (CNTVCT_EL0) и аппаратный генератор RNDR (FEAT_RNG, ID_AA64ISAR0_EL1.RNDR), если он есть. */
#include <stdint.h>
#include "rng.h"

static int have = -1;
static void probe(void) { uint64_t r; __asm__ volatile ("mrs %0, id_aa64isar0_el1" : "=r"(r)); have = ((r >> 60) & 0xF) >= 1; }

uint64_t arch_cycles(void) { uint64_t v; __asm__ volatile ("isb\n mrs %0, cntvct_el0" : "=r"(v)); return v; }

int arch_hw_random(uint64_t* v) {
    if (have < 0) probe();
    if (!have) return 0;
    for (int i = 0; i < 10; i++) {
        uint64_t x, nzcv;
        __asm__ volatile ("mrs %0, s3_3_c2_c4_0\n mrs %1, nzcv" : "=r"(x), "=r"(nzcv));   /* RNDR; Z=1 - нет данных */
        if (!(nzcv & (1u << 30))) { *v = x; return 1; }
    }
    return 0;
}

const char* arch_hw_random_name(void) { if (have < 0) probe(); return have ? "RNDR" : 0; }
