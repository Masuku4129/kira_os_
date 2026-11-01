/* Ожидание событий устройств сном (см. msi.h) и включение MSI / MSI-X в конфигурации PCI. */
#include "rng.h"
#include "msi.h"
#include "sched.h"
#include "vmm.h"

static uint64_t total_fired, total_woken;
void irqwait_stats(uint64_t* fired, uint64_t* woken) { *fired = total_fired; *woken = total_woken; }

void irqwait_fire(irqwait_t* w) {
    rng_event();                                  /* время прерывания устройства - в пул генератора */
    __atomic_add_fetch(&total_fired, 1, __ATOMIC_RELAXED);
    uint64_t f = spin_lock_irqsave(&w->lk);
    __atomic_add_fetch(&w->fired, 1, __ATOMIC_ACQ_REL);
    thread_t* t = (thread_t*)w->waiter;
    w->waiter = 0;
    if (t) { w->wakeups++; __atomic_add_fetch(&total_woken, 1, __ATOMIC_RELAXED); sched_wake_waiter(t); }
    spin_unlock_irqrestore(&w->lk, f);
}

void irqwait_wait(irqwait_t* w, uint32_t mark, uint64_t ticks) {
    uint64_t f = spin_lock_irqsave(&w->lk);
    if (__atomic_load_n(&w->fired, __ATOMIC_ACQUIRE) != mark) { spin_unlock_irqrestore(&w->lk, f); return; }
    w->waiter = (void*)sched_current();
    sched_wait_and_unlock(&w->lk, ticks);                /* замок отпускается, когда поток уже помечен спящим */
    irq_restore(f);
    f = spin_lock_irqsave(&w->lk);
    if (w->waiter == (void*)sched_current()) w->waiter = 0;   /* проснулся по сроку */
    spin_unlock_irqrestore(&w->lk, f);
}

int pci_msix_setup(const pci_dev_t* d, unsigned entry, uint64_t addr, uint32_t data) {
    uint8_t id; uint16_t c = 0;
    while ((c = pci_next_cap(d, c, &id)) != 0 && id != 0x11) { }
    if (!c) return -1;
    uint16_t ctl = pci_read16(d, (uint16_t)(c + 2));
    if (entry > (ctl & 0x7FFu)) return -1;
    uint32_t tbl = pci_read32(d, (uint16_t)(c + 4));
    volatile uint32_t* e = (volatile uint32_t*)pci_map_bar(d, (int)(tbl & 7), (tbl & ~7u) + entry * 16u, 16);
    if (!e) return -1;
    pci_write16(d, (uint16_t)(c + 2), (uint16_t)(ctl | 0x4000));        /* маска функции на время записи */
    e[0] = (uint32_t)addr; e[1] = (uint32_t)(addr >> 32); e[2] = data; e[3] = 0;   /* запись открыта */
    pci_write16(d, 0x04, (uint16_t)(pci_read16(d, 0x04) | PCI_CMD_INTX_OFF));
    pci_write16(d, (uint16_t)(c + 2), (uint16_t)((ctl | 0x8000) & ~0x4000));  /* MSI-X включён, маска снята */
    return 0;
}

int pci_msi_setup(const pci_dev_t* d, uint64_t addr, uint32_t data) {
    uint8_t id; uint16_t c = 0;
    while ((c = pci_next_cap(d, c, &id)) != 0 && id != 0x05) { }
    if (!c) return -1;
    uint16_t ctl = pci_read16(d, (uint16_t)(c + 2));
    pci_write32(d, (uint16_t)(c + 4), (uint32_t)addr);
    if (ctl & 0x80) { pci_write32(d, (uint16_t)(c + 8), (uint32_t)(addr >> 32)); pci_write16(d, (uint16_t)(c + 12), (uint16_t)data); }
    else { if (addr >> 32) return -1; pci_write16(d, (uint16_t)(c + 8), (uint16_t)data); }
    pci_write16(d, 0x04, (uint16_t)(pci_read16(d, 0x04) | PCI_CMD_INTX_OFF));
    pci_write16(d, (uint16_t)(c + 2), (uint16_t)((ctl & ~0x70u) | 1));      /* одно сообщение, включено */
    return 0;
}
