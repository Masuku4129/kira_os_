/* x86-64: векторы 0x50..0x5F для MSI / MSI-X. Сообщение - запись в окно Local APIC (0xFEE00000) с номером ядра
   получателя; доставляется загрузочному ядру. Если Local APIC в итоге не включится, сообщения пропадут, а ожидающий
   драйвер проснётся по сроку (irqwait_wait с таймаутом) - как при опросе. */
#include "../../msi.h"
#include "idt.h"
#include "apic.h"
#include "percpu.h"

#define MSI_BASE 0x50
#define MSI_N    16
extern void* msi_stub_table[MSI_N];
static struct { msi_fn fn; void* ctx; } tab[MSI_N];
static unsigned nvec;
static spinlock_t lk = SPINLOCK_INIT;

int msi_alloc(msi_fn fn, void* ctx, uint64_t* addr, uint32_t* data) {
    uint32_t dest = percpu_table[0].apic_id;
    if (dest > 255) return -1;                              /* в адресе MSI - 8 бит номера ядра */
    uint64_t f = spin_lock_irqsave(&lk);
    if (nvec >= MSI_N) { spin_unlock_irqrestore(&lk, f); return -1; }
    unsigned i = nvec++;
    tab[i].fn = fn; tab[i].ctx = ctx;
    idt_set_gate((uint8_t)(MSI_BASE + i), msi_stub_table[i], 0x08, 0x8E);
    spin_unlock_irqrestore(&lk, f);
    *addr = 0xFEE00000ull | (uint64_t)dest << 12;
    *data = MSI_BASE + i;                                   /* фиксированная доставка, по фронту */
    return 0;
}

/* Из irq_dispatch: вектор MSI. 1 - обработан (EOI - вызывающий). */
int msi_dispatch(uint64_t vector) {
    if (vector < MSI_BASE || vector >= MSI_BASE + MSI_N) return 0;
    unsigned i = (unsigned)(vector - MSI_BASE);
    if (i < nvec && tab[i].fn) tab[i].fn(tab[i].ctx);
    return 1;
}
