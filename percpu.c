#include "percpu.h"

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

#ifdef KIRA_HOST_TEST
void percpu_install(percpu_t* p) { (void)p; }
#endif

void percpu_init_bsp(uint32_t apic_id) {
    ncpu = 0;
    percpu_t* p = percpu_alloc(apic_id);
    percpu_install(p);
}
