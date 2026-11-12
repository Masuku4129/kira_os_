#include "watchdog.h"
#include "cpu.h"
#include "uart.h"
#include "timer.h"
#include "smp.h"
#include "spinlock.h"
#include "sched.h"
#include "percpu.h"
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

/* Called only from virtq_run's bounded device wait. The wait has its own
   hard deadline; allowing the actual caller (including a process performing
   filesystem I/O) to report progress prevents a false global watchdog panic.
   Do not call this from an unbounded driver or general-purpose loop. */
void watchdog_progress(void) {
    watchdog_feed();
}

/* Остановка всей машины - не зависание системы. Хранитель времени тикает всегда (каждые 2 мс), поэтому если между
   двумя проверками прошло больше 200 мс, машина стояла целиком: хозяин приостановил виртуальную машину (эмулятор
   на телефоне, перегруженный сервер) или все ядра одновременно были с выключенными прерываниями. Время TSC при
   этом ушло вперёд, но ни один поток не мог исполняться - этот промежуток не засчитывается кормящему потоку. */
#define WATCHDOG_STALL (TIMER_HZ / 5)
static volatile uint64_t last_check_tick;
volatile uint64_t watchdog_stalls, watchdog_stall_max;     /* сколько раз и на сколько тиков машина стояла */
#define WD_TRACE 6
uint64_t watchdog_stall_where[WD_TRACE];                    /* где застал ядро первый тик после самой долгой остановки */
uint32_t watchdog_stall_cpu;

void watchdog_check(void) {
    uint64_t now = timer_get_ticks();
    uint64_t prev = __atomic_load_n(&last_check_tick, __ATOMIC_ACQUIRE);
    while (now > prev && !__atomic_compare_exchange_n(&last_check_tick, &prev, now, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { }
    if (prev && now > prev && now - prev > WATCHDOG_STALL) {
        __atomic_fetch_add(&last_feed_tick, now - prev, __ATOMIC_ACQ_REL);
        __atomic_fetch_add(&watchdog_stalls, 1, __ATOMIC_RELAXED);
        if (now - prev > watchdog_stall_max) {
            /* Прерывание таймера приходит сразу, как только ядро снова включило прерывания: место, где оно застало
               ядро, - конец участка с выключенными прерываниями (если машину не останавливал хозяин). */
            percpu_t* me = this_cpu();
            watchdog_stall_max = now - prev;
            watchdog_stall_where[0] = me->irq_rip;
            unsigned k = sched_trace_current(me->irq_rbp, &watchdog_stall_where[1], WD_TRACE - 1);
            for (k++; k < WD_TRACE; k++) watchdog_stall_where[k] = 0;
            watchdog_stall_cpu = me->cpu_id;
        }
    }
    uint64_t last = __atomic_load_n(&last_feed_tick, __ATOMIC_ACQUIRE);
    if (timer_get_ticks() - last > WATCHDOG_TIMEOUT_LIMIT) {
        watchdog_trigger_panic("Watchdog timeout: System thread hang detected!");
    }
}

void watchdog_trigger_panic(const char* reason) {
    cpu_irq_off();
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
    for (;;) cpu_halt();
}
