#include "futex.h"
#include "kobj.h"
#include "sched.h"
#include "spinlock.h"
#include "timer.h"

#define BUCKETS 64

typedef struct fwaiter { uint64_t key; thread_t* t; struct fwaiter* next; int queued; } fwaiter_t;
typedef struct { spinlock_t lk; fwaiter_t* head; } bucket_t;

static bucket_t buckets[BUCKETS];

static bucket_t* bucket_of(uint64_t key) { return &buckets[((key >> 2) * 0x9E3779B97F4A7C15ULL) >> 58]; }

int futex_wait(uint64_t key, uint64_t word, int (*load)(uint64_t word, uint32_t* v), uint32_t expected, uint64_t ticks, int (*stop)(void)) {
    bucket_t* b = bucket_of(key);
    uint64_t f = spin_lock_irqsave(&b->lk);
    uint32_t v;
    if (load(word, &v) != 0) { spin_unlock_irqrestore(&b->lk, f); return KE_ARG; }
    if (v != expected) { spin_unlock_irqrestore(&b->lk, f); return KE_AGAIN; }
    if (stop && stop()) { spin_unlock_irqrestore(&b->lk, f); return KE_INTR; }
    if (ticks == 0) { spin_unlock_irqrestore(&b->lk, f); return KE_TIMEOUT; }
    fwaiter_t w = { key, (thread_t*)sched_current(), b->head, 1 };
    b->head = &w;
    sched_wait_and_unlock(&b->lk, ticks);
    irq_restore(f);
    f = spin_lock_irqsave(&b->lk);
    int rc = 0;
    if (w.queued) {                                     /* не разбудили: срок вышел или процесс завершается */
        for (fwaiter_t** pp = &b->head; *pp; pp = &(*pp)->next) if (*pp == &w) { *pp = w.next; break; }
        rc = (stop && stop()) ? KE_INTR : KE_TIMEOUT;
    }
    spin_unlock_irqrestore(&b->lk, f);
    return rc;
}

unsigned futex_wake(uint64_t key, unsigned count) {
    bucket_t* b = bucket_of(key);
    unsigned n = 0;
    uint64_t f = spin_lock_irqsave(&b->lk);
    for (fwaiter_t** pp = &b->head; *pp && n < count; ) {
        fwaiter_t* w = *pp;
        if (w->key != key) { pp = &w->next; continue; }
        *pp = w->next;
        w->queued = 0;
        sched_wake_waiter(w->t);                        /* под замком корзины: ждущий не уйдёт раньше */
        n++;
    }
    spin_unlock_irqrestore(&b->lk, f);
    return n;
}
