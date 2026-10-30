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
static int anchored;
static spinlock_t tick_lock = SPINLOCK_INIT;     /* общее время обновляют все ядра: каждое получает тик своего LAPIC */

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
    spin_lock_raw(&tick_lock);                       /* в обработчике прерывания IF уже 0 */
    /* Часы TSC точнее счёта прерываний: если эмулятор или нагрузка потеряли часть прерываний таймера,
       тики догоняют реальное время, и сон, кванты по времени и «жизнь» не отстают. */
    if (clock_ready()) {
        if (!anchored) { anchor_ns = clock_ns(); anchor_ticks = ticks; anchored = 1; ticks++; }
        else {
            uint64_t d = anchor_ticks + (clock_ns() - anchor_ns) / (1000000000ULL / TIMER_HZ);
            if (d > ticks) ticks = d;
        }
    } else {
        ticks++;
    }
    spin_unlock_raw(&tick_lock);
    uart_tick_flush();
    watchdog_check();
    sched_tick(); // пробуждение спящих, учёт кванта; само вытеснение - после EOI
}

uint64_t timer_get_irqs(void) { return irqs; }

uint64_t timer_get_ticks(void) {
    return ticks;
}
