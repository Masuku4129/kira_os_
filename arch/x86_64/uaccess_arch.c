#include "uaccess.h"

extern char uaccess_begin[], uaccess_end[], uaccess_fault[];
extern volatile uint8_t uaccess_smap;

int uaccess_fixup(uint64_t* frame, uint64_t rip) {
    if (rip < (uint64_t)(uintptr_t)uaccess_begin || rip >= (uint64_t)(uintptr_t)uaccess_end) return 0;
    frame[17] = (uint64_t)(uintptr_t)uaccess_fault;                /* RIP в кадре исключения (см. crashdump.h) */
    return 1;
}

int uaccess_enable_smap(void) {
    uint32_t a, b, c, d;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
    if (a < 7) return 0;
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));
    if (!(b & (1u << 20))) return 0;
    uint64_t cr4; __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4 | (1ULL << 21)) : "memory");
    uaccess_smap = 1;
    return 1;
}
