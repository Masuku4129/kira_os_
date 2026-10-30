#include "timer.h"
#include "pic.h"
#include "watchdog.h"
#include "sched.h"
#include "uart.h"
#include "clock.h"
#include "percpu.h"
#include "spinlock.h"

#define PIT_CHANNEL0_PORT 0x40
#define PIT_COMMAND_PORT  0x43

static volatile uint64_t ticks = 0;
static volatile uint64_t irqs = 0;
static uint64_t anchor_ns, anchor_ticks;
static volatile int anchored;
static spinlock_t tick_lock = SPINLOCK_INIT;     /* только для однократной установки точки отсчёта */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

int timer_init(uint32_t frequency) {
    // PIT: базовая частота 1193182 Гц, делитель 16-битный (0 означает 65536)
    if (frequency < 19 || frequency > 1193182) {
        return -1;
    }
    uint32_t divisor = 1193182 / frequency;

    outb(PIT_COMMAND_PORT, 0x36); // Channel 0, lobyte/hibyte, rate generator
    outb(PIT_CHANNEL0_PORT, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0_PORT, (uint8_t)((divisor >> 8) & 0xFF));

    if (irq_register_handler(0, timer_handler) != 0) {
        return -1;
    }
    pic_unmask_irq(0);

    uart_print("[KIRA TIMER] PIT started, frequency ");
    uart_print_dec(1193182 / divisor);
    uart_print(" Hz, IRQ0 unmasked.\n");
    return 0;
}

// Вызывается из irq_dispatch; EOI отправляет диспетчер.
void timer_handler(void) {
    /* Ядра равнозначны: каждое получает тик своего таймера LAPIC и обновляет общее время (оно выводится из TSC и
       только растёт, поэтому порядок ядер не важен). Падение любого ядра не оставляет систему без часов. */
    __atomic_fetch_add(&irqs, 1, __ATOMIC_RELAXED);
    /* Часы TSC точнее счёта прерываний: если эмулятор или нагрузка потеряли часть прерываний таймера,
       тики догоняют реальное время, и сон, кванты по времени и «жизнь» не отстают.
       Общий счётчик двигают все ядра без замка (сравнение-обмен, только вперёд): тик не должен стоять в очереди
       за другими ядрами - на виртуальной машине, где ядер больше, чем у хозяина, такая очередь растягивалась. */
    if (clock_ready()) {
        if (!__atomic_load_n(&anchored, __ATOMIC_ACQUIRE)) {
            spin_lock_raw(&tick_lock);                   /* один раз за жизнь: точка отсчёта */
            if (!anchored) {
                anchor_ns = clock_ns(); anchor_ticks = ticks + 1;
                __atomic_store_n(&ticks, anchor_ticks, __ATOMIC_RELEASE);
                __atomic_store_n(&anchored, 1, __ATOMIC_RELEASE);
            }
            spin_unlock_raw(&tick_lock);
        } else {
            uint64_t d = anchor_ticks + (clock_ns() - anchor_ns) / (1000000000ULL / TIMER_HZ);
            uint64_t cur = __atomic_load_n(&ticks, __ATOMIC_RELAXED);
            while (d > cur && !__atomic_compare_exchange_n(&ticks, &cur, d, 1, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) { }
        }
    } else {
        __atomic_fetch_add(&ticks, 1, __ATOMIC_RELEASE);
    }
    uart_tick_flush();
    watchdog_check();
    sched_tick(); // пробуждение спящих, учёт кванта; само вытеснение - после EOI
}

uint64_t timer_get_irqs(void) { return irqs; }
void timer_lock_state(uint32_t* next, uint32_t* owner) { *next = tick_lock.next; *owner = tick_lock.owner; }

uint64_t timer_get_ticks(void) {
    return ticks;
}
