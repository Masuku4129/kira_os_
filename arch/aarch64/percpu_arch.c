#include "percpu.h"

/* ARM64 keeps the current CPU's kernel-local state in TPIDR_EL1.
   Install it before any code path (notably spinlocks and the kernel log)
   dereferences this_cpu(). */
void percpu_install(percpu_t* p) {
    __asm__ volatile ("msr tpidr_el1, %0\n isb" : : "r"(p) : "memory");
}
