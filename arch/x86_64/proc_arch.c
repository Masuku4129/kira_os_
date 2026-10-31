#include "sched.h"
#include "percpu.h"
#include "vmm.h"
#include "gdt.h"

/* x86-64: Вызывается планировщиком перед каждым переключением потоков (прерывания выключены): стек ядра для входа из
   Ring 3 (TSS.rsp0 и SYSCALL) и адресное пространство (CR3). */
void proc_arch_switch(const thread_t* next) {
    if (next->kstack_top) {
        tss_set_rsp0(next->kstack_top);        /* куда CPU положит кадр при входе из Ring 3 */
        this_cpu()->syscall_kernel_rsp = next->kstack_top; /* стек для SYSCALL этого ядра */
    }
    uint64_t want = next->cr3 ? next->cr3 : vmm_kernel_root();
    if (want != this_cpu()->active_cr3) {
        /* Сначала объявить, потом загрузить: рассылка сброса TLB (vmm_tlb_shootdown) смотрит на active_cr3 других
           ядер; ядро, которое ещё не объявило новое пространство, загрузит его уже с новыми таблицами. */
        __atomic_store_n(&this_cpu()->active_cr3, want, __ATOMIC_SEQ_CST);
        vmm_switch(want);
    }
}

