#include "kernel.h"
#include "kernel_boot.h"
#include "uart.h"
#include "smp.h"
#include "spinlock.h"
#include "earlycon.h"
#include "display.h"
#include "console.h"
#include "progs.h"
#include "kdrv.h"
#include "life.h"
#include "timer.h"
#include "mind_sys.h"
#include "experience.h"
#include "sched.h"
#include "immune.h"
#include "watchdog.h"

#define SELF_CHECK_PERIOD_S 5

void kernel_panic(const char* reason) {
    __asm__ volatile ("cli");
    smp_halt_others();
    spin_bust_locks();
    earlycon_alert();
    uart_print("\n[KIRA KERNEL PANIC] ");
    uart_print(reason);
    uart_print("\n[KIRA KERNEL PANIC] called from ");
    uart_print_hex((uint64_t)(uintptr_t)__builtin_return_address(0));
    uart_print("\n");
    uart_flush_all();
    display_panic_flush();
    for (;;) __asm__ volatile ("hlt");
}

/* kernel_main ABI is unchanged; boot composition is isolated from steady-state work. */
void kernel_main(unsigned long magic, unsigned long addr) {
    kernel_boot_init(magic, addr);

    uint64_t handled = 0;
    uint64_t seconds = 0;
    uint64_t last_verify = 0;

    for (;;) {
        console_poll();
        prog_poll();
        kdrv_poll();
        life_tick();

        uint64_t now = timer_get_ticks();
        unsigned pending = 0;
        while (handled < now) {
            handled++;
            if (handled % TIMER_HZ == 0) pending++;
        }

        if (pending) {
            seconds += pending;
            (void)console_take_events();
            mind_sys_tick(seconds);
            experience_tick(seconds);
            sched_reap();

            if (seconds - last_verify >= SELF_CHECK_PERIOD_S) {
                last_verify = seconds;
                if (immune_verify_self() != 0)
                    kernel_panic("IMMUNE: kernel code/rodata digest mismatch (self-integrity violated)");
            }
        }

        watchdog_feed();
        {
            extern volatile uint64_t gs_fixups, gs_fixup_rip;
            extern volatile uint32_t gs_fixup_vec;
            static uint64_t seen_fix;
            uint64_t n = gs_fixups;
            if (n != seen_fix) {
                seen_fix = n;
                uart_print("[KIRA GS] kernel entered with the process GS: vector ");
                uart_print_dec(gs_fixup_vec);
                uart_print(" at RIP ");
                uart_print_hex(gs_fixup_rip);
                uart_print(", times ");
                uart_print_dec(n);
                uart_print("\n");
            }
        }
        {
            extern volatile uint64_t watchdog_stalls, watchdog_stall_max;
            static uint64_t seen_stalls;
            uint64_t n = watchdog_stalls;
            if (n != seen_stalls) {
                seen_stalls = n;
                uart_print("[KIRA WATCHDOG] the whole machine stood still (longest ");
                uart_print_dec(watchdog_stall_max * 1000 / TIMER_HZ);
                uart_print(" ms, times ");
                uart_print_dec(n);
                uart_print("): not counted as a hang. Either the host paused the VM or this code ran with interrupts off: core ");
                extern uint64_t watchdog_stall_where[];
                extern uint32_t watchdog_stall_cpu;
                uart_print_dec(watchdog_stall_cpu);
                uart_print(" at");
                for (int k = 0; k < 6 && watchdog_stall_where[k]; k++) {
                    uart_print(" ");
                    uart_print_hex(watchdog_stall_where[k]);
                }
                uart_print("\n");
            }
        }
        sched_sleep_ticks(TIMER_PER_10MS);
    }
}
