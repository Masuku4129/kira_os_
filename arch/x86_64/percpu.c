#include "percpu.h"

#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

/* Запись загрузочного ядра задана статически: GS указывает на неё с первой строки kernel_main, ещё до uart_init. */
percpu_t percpu_table[PERCPU_MAX_CPUS] = { [0] = { .self = &percpu_table[0] } };
static unsigned ncpu;
#ifdef KIRA_HOST_TEST
unsigned host_cpu;
#endif

_Static_assert(__builtin_offsetof(percpu_t, self) == 0, "percpu.self must be at 0 (mov %gs:0)");
_Static_assert(__builtin_offsetof(percpu_t, syscall_user_rsp) == PERCPU_OFF_USER_RSP, "offset used by interrupts.s");
_Static_assert(__builtin_offsetof(percpu_t, syscall_kernel_rsp) == PERCPU_OFF_KERNEL_RSP, "offset used by interrupts.s");

percpu_t* percpu_alloc(uint32_t apic_id) {
    if (ncpu >= PERCPU_MAX_CPUS) return 0;
    percpu_t* p = &percpu_table[ncpu];
    p->self = p;
    p->cpu_id = ncpu++;
    p->apic_id = apic_id;
    return p;
}

unsigned percpu_count(void) { return ncpu; }

void percpu_set_gs(percpu_t* p) {
#ifndef KIRA_HOST_TEST
    uint64_t v = (uint64_t)(uintptr_t)p;
    __asm__ volatile ("wrmsr" : : "c"(MSR_GS_BASE), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
    __asm__ volatile ("wrmsr" : : "c"(MSR_KERNEL_GS_BASE), "a"(0), "d"(0));
#else
    (void)p;
#endif
}

void percpu_init_bsp(uint32_t apic_id) {
    ncpu = 0;
    percpu_t* p = percpu_alloc(apic_id);
    percpu_set_gs(p);
}
