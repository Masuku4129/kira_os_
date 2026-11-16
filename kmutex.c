#include "kmutex.h"
#include "sched.h"
#include "timer.h"
#include "bbring.h"

extern void kernel_panic(const char* reason) __attribute__((noreturn));

static char early_owner;      // владелец до запуска планировщика (одно ядро, потоков ещё нет)

static int real(const thread_t* t) { return t != (const thread_t*)&early_owner; }

static thread_t* me(void) {
    thread_t* t = (thread_t*)sched_current();
    return t ? t : (thread_t*)&early_owner;
}

void kmutex_lock(kmutex_t* m) {
    thread_t* t = me();
    uint64_t f = spin_lock_irqsave(&m->lk);
    if (!m->owner) {
        m->owner = t;
        if (real(t)) t->mutexes_held++;
        spin_unlock_irqrestore(&m->lk, f);
        return;
    }
    if (m->owner == t) {
        spin_unlock_irqrestore(&m->lk, f);
        kernel_panic("kmutex: recursive lock (deadlock)");
    }
    t->wait_next = 0;
    if (m->tail) m->tail->wait_next = t; else m->head = t;
    m->tail = t;
    uint64_t t_block = timer_get_ticks();
    uint32_t owner_id = m->owner && real(m->owner) ? m->owner->id : 0;
    // Спим, пока владелец не передаст мьютекс. Замок m->lk отпускается внутри, уже после пометки "ждёт".
    sched_block_and_unlock(&m->lk);
    irq_restore(f);                          // здесь мьютекс уже наш (передан в kmutex_unlock)
    {   /* чёрный ящик: ожидание мьютекса дольше 50 мс (кто держал - номер потока-владельца) */
        uint64_t ms = (timer_get_ticks() - t_block) * 1000 / TIMER_HZ;
        if (real(t) && ms >= 50) {
            static bb_gate_t gate; uint32_t skipped;
            if (bb_gate_pass(&gate, timer_get_ticks() * 1000 / TIMER_HZ, 10, &skipped))
                bb_event(BB_LOCK, ms >= 1000 ? BB_SEV_SEVERE : ms >= 200 ? BB_SEV_WARN : BB_SEV_INFO, (uint32_t)(ms * 1000), 0xFFFF, owner_id, t->last_cpu, skipped > 255 ? 255 : (uint8_t)skipped, t->name);
            else bb_note_suppressed(BB_LOCK, (uint32_t)(ms * 1000));
        }
    }
}

int kmutex_trylock(kmutex_t* m) {
    uint64_t f = spin_lock_irqsave(&m->lk);
    int ok = 0;
    if (!m->owner) { thread_t* t = me(); m->owner = t; if (real(t)) t->mutexes_held++; ok = 1; }
    spin_unlock_irqrestore(&m->lk, f);
    return ok;
}

void kmutex_unlock(kmutex_t* m) {
    uint64_t f = spin_lock_irqsave(&m->lk);
    thread_t* w = m->head;
    thread_t* self = me();
    if (real(self) && self->mutexes_held) self->mutexes_held--;
    if (w) {                                  // передаём первому в очереди, минуя «свободное» состояние
        m->head = w->wait_next;
        if (!m->head) m->tail = 0;
        w->wait_next = 0;
        m->owner = w;
        w->mutexes_held++;
    } else {
        m->owner = 0;
    }
    spin_unlock_irqrestore(&m->lk, f);
    if (w) sched_wake(w);
}

int kmutex_held(const kmutex_t* m) { return m->owner == me(); }
