#include "watchdog.h"
#include "uart.h"
#include "timer.h"
#include "smp.h"
#include "spinlock.h"
#include "sched.h"
#include "display.h"

// Тики таймера (TIMER_HZ): 5 секунд без watchdog_feed() => паника.
// Это программный watchdog, а не аппаратный (iTCO/WDT). Срок считается по общему времени (тики выводятся из TSC),
// а не по числу вызовов: проверку делает таймер каждого ядра, и частота вызовов от числа ядер не зависит.
// Поэтому зависание ядра, на котором работает кормящий поток, замечает любое другое ядро.
#define WATCHDOG_TIMEOUT_LIMIT (5 * TIMER_HZ)

static volatile uint64_t last_feed_tick;

void watchdog_init(void) {
    __atomic_store_n(&last_feed_tick, timer_get_ticks(), __ATOMIC_RELEASE);
    uart_print("[KIRA WATCHDOG] Software watchdog armed (5 s timeout, fed by main loop, checked by every core).\n");
}

void watchdog_feed(void) {
    __atomic_store_n(&last_feed_tick, timer_get_ticks(), __ATOMIC_RELEASE);
}

void watchdog_check(void) {
    uint64_t last = __atomic_load_n(&last_feed_tick, __ATOMIC_ACQUIRE);
    if (timer_get_ticks() - last > WATCHDOG_TIMEOUT_LIMIT) {
        watchdog_trigger_panic("Watchdog timeout: System thread hang detected!");
    }
}

void watchdog_trigger_panic(const char* reason) {
    __asm__ volatile ("cli");
    smp_halt_others();                // остановить остальные ядра: иначе «система остановлена» было бы неправдой
    spin_bust_locks();                // замок мог держать зависший код: отчёт не должен на нём встать
    sched_crash_wait_halted();        // ядра записывают, где стояли: отчёт покажет место зависания
    uart_print("\n[KIRA KERNEL PANIC] Fatal: ");
    uart_print(reason);
    uart_print("\n[KIRA WATCHDOG] last feed at tick ");
    uart_print_dec(__atomic_load_n(&last_feed_tick, __ATOMIC_ACQUIRE));
    uart_print(". State at the moment of the hang:\n");
    { uint32_t n, o; timer_lock_state(&n, &o);
      uart_print("[KIRA WATCHDOG] tick_lock next "); uart_print_dec(n); uart_print(" owner "); uart_print_dec(o); uart_print("\n"); }
    sched_crash_report(uart_print);
    uart_print("[KIRA WATCHDOG] Halting execution immediately.\n");
    uart_flush_all();
    display_panic_flush();
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
