#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>
#include "cpu.h"
#ifndef KIRA_HOST_TEST
#include "percpu.h"
/* Считаем замки на ядро: сбой на ядре, держащем замок, нельзя изолировать (защищаемые данные могли остаться
   недописанными), он остаётся аварией всей системы. */
#define SPIN_HELD_INC() (this_cpu()->locks_held++)
#define SPIN_HELD_DEC() (this_cpu()->locks_held--)
#else
#define SPIN_HELD_INC() ((void)0)
#define SPIN_HELD_DEC() ((void)0)
#endif

// Билетная блокировка с запретом прерываний на время удержания. Билеты выдаются по порядку,
// поэтому ожидающие получают замок в порядке прихода (никто не голодает), а число ядер
// не меняет правил. На одном ядре замок никогда не занят чужим кодом: прерывания выключены,
// ожидание невозможно, остаётся стоимость двух атомарных операций.
//
// Правила:
//  - замки не вложены друг в друга, кроме явного порядка (сейчас: uart -> klog, uart -> earlycon);
//  - под замком нельзя спать, переключать контекст и печатать через uart_print (кроме самого uart);
//  - один и тот же замок повторно не берётся (взаимная блокировка).
// Паника отменяет замки (spin_bust): аварийный вывод не должен зависнуть на замке, который
// держал код, упавший с исключением.
typedef struct {
    volatile uint32_t next;     // следующий выдаваемый билет
    volatile uint32_t owner;    // билет, которому сейчас разрешено работать
} spinlock_t;

#define SPINLOCK_INIT { 0, 0 }

__attribute__((weak)) volatile int spin_bust = 0;

static inline void spin_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile ("pause");
#endif
}

static inline uint64_t spin_lock_irqsave(spinlock_t* l) {
    uint64_t f = irq_save();
    if (spin_bust) return f;
    uint32_t t = __atomic_fetch_add(&l->next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != t) spin_relax();
    SPIN_HELD_INC();
    return f;
}

static inline void spin_unlock_irqrestore(spinlock_t* l, uint64_t f) {
    if (!spin_bust) SPIN_HELD_DEC();
    if (!spin_bust) __atomic_store_n(&l->owner, __atomic_load_n(&l->owner, __ATOMIC_RELAXED) + 1, __ATOMIC_RELEASE);
    irq_restore(f);
}

// Без запрета прерываний: вызывающий уже выключил их сам (планировщик передаёт замок новому потоку).
static inline void spin_lock_raw(spinlock_t* l) {
    if (spin_bust) return;
    uint32_t t = __atomic_fetch_add(&l->next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != t) spin_relax();
    SPIN_HELD_INC();
}

static inline void spin_unlock_raw(spinlock_t* l) {
    if (!spin_bust) SPIN_HELD_DEC();
    if (!spin_bust) __atomic_store_n(&l->owner, __atomic_load_n(&l->owner, __ATOMIC_RELAXED) + 1, __ATOMIC_RELEASE);
}

static inline void spin_bust_locks(void) { spin_bust = 1; }

#endif
