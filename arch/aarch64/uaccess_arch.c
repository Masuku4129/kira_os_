/* ARM64: копирование между ядром и программой - uaccess.S (LDTR/STTR), перехват сбоя внутри него, PAN. */
#include "uaccess.h"
#include "sched.h"
#include "percpu.h"
#include "vmm.h"

extern char uaccess_begin[], uaccess_end[], uaccess_fault[];
int uaccess_copy_from(void* dst, const void* usrc, uint64_t n);
int uaccess_copy_to(uint64_t udst, const void* src, uint64_t n);
void arm_enter_user(uint64_t pc, uint64_t sp, uint64_t arg, uint64_t kstack_top);

/* Общий uaccess.c зовёт одну функцию копирования для обоих направлений: направление - по тому, какой адрес
   принадлежит программе. */
int uaccess_copy(void* dst, const void* src, uint64_t n) {
    if ((uint64_t)(uintptr_t)src >= VMM_USER_BASE) return uaccess_copy_from(dst, src, n);
    return uaccess_copy_to((uint64_t)(uintptr_t)dst, src, n);
}

/* Кадр исключения: x0..x30, ELR (31), SPSR, ESR, FAR. 1 - сбой внутри копирования, продолжить с -1. */
int uaccess_fixup(uint64_t* frame, uint64_t pc) {
    if (pc < (uint64_t)(uintptr_t)uaccess_begin || pc >= (uint64_t)(uintptr_t)uaccess_end) return 0;
    frame[31] = (uint64_t)(uintptr_t)uaccess_fault;
    return 1;
}

/* PAN (ARMv8.1): ядру закрыты страницы программ для обычных команд. 1 - включён. */
int uaccess_enable_smap(void) {
    uint64_t mmfr1;
    __asm__ volatile ("mrs %0, id_aa64mmfr1_el1" : "=r"(mmfr1));
    if (((mmfr1 >> 20) & 0xF) == 0) return 0;
    uint64_t sctlr;
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr &= ~(1ull << 23);                                   /* SPAN = 0: PAN ставится сам при каждом входе в EL1 */
    __asm__ volatile ("msr sctlr_el1, %0\n isb\n .inst 0xd500419f\n isb" : : "r"(sctlr) : "memory");   /* msr pan, #1 */
    return 1;
}

void enter_user(uint64_t pc, uint64_t sp) {
    arm_enter_user(pc, sp, 0, sched_current()->kstack_top);
    for (;;) { }
}
void enter_user_arg(uint64_t pc, uint64_t sp, uint64_t arg) {
    arm_enter_user(pc, sp, arg, sched_current()->kstack_top);
    for (;;) { }
}
