#include "syscall.h"
#include "uart.h"
#include "proc.h"
#include "cpu.h"

/* x86-64: вход SYSCALL/SYSRET (MSR каждого процессора). */
#define MSR_EFER  0xC0000080
#define MSR_STAR  0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084

extern void syscall_entry_asm(void);

void syscall_init_cpu(void) {
    cpu_wrmsr(MSR_EFER, cpu_rdmsr(MSR_EFER) | 1);                 // EFER.SCE
    // SYSCALL: CS=0x08, SS=0x10. SYSRET64: SS=0x13+8=0x1B, CS=0x13+16=0x23 (порядок data->code в GDT).
    // База SYSRET - с RPL 3 (0x13): Intel сам ставит RPL=3 в SS, а AMD берёт биты из STAR как есть - с базой 0x10
    // программа на AMD жила с SS=0x18 (RPL 0), и первый же iretq из прерывания в неё давал #GP (стенд, EPYC).
    cpu_wrmsr(MSR_STAR, ((uint64_t)0x08 << 32) | ((uint64_t)0x13 << 48));
    cpu_wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry_asm);
    cpu_wrmsr(MSR_FMASK, 0x40700);                                // сбросить IF, TF, DF, AC при входе (IF обратно включает обработчик после смены стека)
}

void syscall_init(void) {
    syscall_init_cpu();
    uart_print("[KIRA SYSCALL] SYSCALL/SYSRET enabled: write, exit, ticks, sleep, yield, getpid, gfx, fs (sandbox /data/<name>), heap (quota), threads, futex, ports, shared memory, clock; user pointers validated.\n");
}

// SYSCALL выполнен с последнего байта пользовательской половины: адрес возврата неканонический, SYSRET
// на него нельзя (исключение случилось бы в Ring 0 с пользовательским стеком). Процесс завершается.
void syscall_bad_return(void) {
    uart_print("[KIRA SYSCALL] pid "); uart_print_dec(proc_current_pid());
    uart_print(": syscall from the top of the user half (non-canonical return address): process terminated.\n");
    proc_exit(-1);
}

