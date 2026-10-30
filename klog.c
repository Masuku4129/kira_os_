#include "klog.h"
#include "cpu.h"
#include "spinlock.h"

static char ring[KLOG_SIZE];
static volatile uint64_t total;
static spinlock_t klog_lock = SPINLOCK_INIT;

void klog_putc(char c) {
    uint64_t f = spin_lock_irqsave(&klog_lock);
    ring[total % KLOG_SIZE] = c;
    total++;
    spin_unlock_irqrestore(&klog_lock, f);
}

uint64_t klog_total(void) { return total; }

unsigned klog_read(uint64_t* cursor, char* out, unsigned max, unsigned* lost) {
    uint64_t f = spin_lock_irqsave(&klog_lock);
    uint64_t t = total;
    if (lost) *lost = 0;
    if (t - *cursor > KLOG_SIZE) {                  // читатель отстал на целый круг
        *cursor = t - KLOG_SIZE;
        if (lost) *lost = 1;
    }
    unsigned n = 0;
    while (*cursor < t && n < max) out[n++] = ring[(*cursor)++ % KLOG_SIZE];
    spin_unlock_irqrestore(&klog_lock, f);
    return n;
}
