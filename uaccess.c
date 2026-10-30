#include "uaccess.h"
#include "vmm.h"
#include "proc.h"

extern char uaccess_begin[], uaccess_end[], uaccess_fault[];
extern volatile uint8_t uaccess_smap;
int uaccess_copy(void* dst, const void* src, uint64_t n);
int uaccess_read32(const volatile uint32_t* user, uint32_t* out);

int copy_from_user(void* dst, uint64_t usrc, uint64_t n) {
    if (n == 0) return 0;
    if (!vmm_user_range_ok(usrc, n, 0) && (proc_stack_prepare(usrc, n) != 0 || !vmm_user_range_ok(usrc, n, 0))) return -1;
    return uaccess_copy(dst, (const void*)(uintptr_t)usrc, n);
}

int copy_to_user(uint64_t udst, const void* src, uint64_t n) {
    if (n == 0) return 0;
    if (!vmm_user_range_ok(udst, n, 1) && (proc_stack_prepare(udst, n) != 0 || !vmm_user_range_ok(udst, n, 1))) return -1;
    return uaccess_copy((void*)(uintptr_t)udst, src, n);
}

int user_range(uint64_t va, uint64_t len, int write) {
    if (vmm_user_range_ok(va, len, write)) return 1;
    return proc_stack_prepare(va, len) == 0 && vmm_user_range_ok(va, len, write);
}

int user_read32(uint64_t uaddr, uint32_t* out) {
    if ((uaddr & 3) || !vmm_user_range_ok(uaddr, 4, 0)) return -1;
    return uaccess_read32((const volatile uint32_t*)(uintptr_t)uaddr, out);
}

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
