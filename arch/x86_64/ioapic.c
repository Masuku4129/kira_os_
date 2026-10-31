#include "ioapic.h"

#ifndef KIRA_HOST_TEST
#include "vmm.h"
#include "uart.h"
#endif

#define IOREGSEL 0x00
#define IOWIN    0x10
#define REG_ID   0x00
#define REG_VER  0x01
#define REG_REDIR(n) (0x10 + 2 * (n))

irq_line_t ioapic_resolve(const acpi_info_t* a, unsigned irq) {
    irq_line_t l = { irq, 0, 0 };           /* ISA по умолчанию: GSI = IRQ, активный высокий, фронт */
    if (irq < 16 && a->iso_valid[irq]) {
        l.gsi = a->iso_gsi[irq];
        uint16_t f = a->iso_flags[irq];
        if ((f & 3) == 3) l.active_low = 1;
        if (((f >> 2) & 3) == 3) l.level = 1;
    }
    return l;
}

void ioapic_encode(uint8_t vector, int active_low, int level, int masked, uint32_t dest, uint32_t* lo, uint32_t* hi) {
    uint32_t v = vector;                    /* режим доставки 000 (фиксированный), назначение физическое */
    if (active_low) v |= 1u << 13;
    if (level)      v |= 1u << 15;
    if (masked)     v |= 1u << 16;
    *lo = v;
    *hi = (dest & 0xFF) << 24;
}

#ifndef KIRA_HOST_TEST
static volatile uint32_t* base[ACPI_MAX_IOAPIC];
static unsigned nent[ACPI_MAX_IOAPIC];
static unsigned count;

static uint32_t rd(unsigned i, uint32_t reg) { base[i][IOREGSEL / 4] = reg; return base[i][IOWIN / 4]; }
static void wr(unsigned i, uint32_t reg, uint32_t v) { base[i][IOREGSEL / 4] = reg; base[i][IOWIN / 4] = v; }

int ioapic_init(const acpi_info_t* a) {
    count = 0;
    for (unsigned i = 0; i < a->ioapic_count && i < ACPI_MAX_IOAPIC; i++) {
        uint64_t pa = a->ioapic[i].addr;
        volatile void* m = vmm_map_mmio(pa, 0x20, VMM_MMIO_UC);
        if (!m) {
            uart_print("[KIRA IOAPIC] cannot map MMIO page.\n");
            return -1;
        }
        base[i] = (volatile uint32_t*)m;
        count++;
        uint32_t ver = rd(i, REG_VER);
        unsigned maxr = (ver >> 16) & 0xFF;
        if ((ver & 0xFF) == 0 || (ver & 0xFF) == 0xFF || maxr < 7 || maxr > 239) {
            uart_print("[KIRA IOAPIC] version register looks wrong: ");
            uart_print_hex(ver); uart_print("\n");
            return -2;
        }
        nent[i] = maxr + 1;
        uart_print("[KIRA IOAPIC] id "); uart_print_dec(a->ioapic[i].id);
        uart_print(" at "); uart_print_hex(pa);
        uart_print(", version "); uart_print_hex(ver & 0xFF);
        uart_print(", "); uart_print_dec(nent[i]); uart_print(" pins, GSI base ");
        uart_print_dec(a->ioapic[i].gsi_base); uart_print("\n");
    }
    return count ? 0 : -3;
}

static int find(const acpi_info_t* a, uint32_t gsi, unsigned* idx, unsigned* pin) {
    for (unsigned i = 0; i < count; i++) {
        uint32_t b = a->ioapic[i].gsi_base;
        if (gsi >= b && gsi < b + nent[i]) { *idx = i; *pin = gsi - b; return 0; }
    }
    return -1;
}

int ioapic_route_isa(const acpi_info_t* a, unsigned irq, uint32_t dest) {
    irq_line_t l = ioapic_resolve(a, irq);
    unsigned i, p;
    if (find(a, l.gsi, &i, &p) != 0) return -1;
    uint32_t lo, hi;
    ioapic_encode((uint8_t)(32 + irq), l.active_low, l.level, 1, dest, &lo, &hi);
    wr(i, REG_REDIR(p) + 1, hi);
    wr(i, REG_REDIR(p), lo);                       /* пока замаскирована */
    /* запись дошла? (биты 12 и 14 - статус доставки и remote IRR, только для чтения) */
    if ((rd(i, REG_REDIR(p)) & ~0x5000u) != (lo & ~0x5000u) || rd(i, REG_REDIR(p) + 1) != hi) return -2;
    ioapic_encode((uint8_t)(32 + irq), l.active_low, l.level, 0, dest, &lo, &hi);
    wr(i, REG_REDIR(p), lo);                       /* размаскировать */
    return 0;
}

void ioapic_mask_isa(const acpi_info_t* a, unsigned irq, int masked) {
    irq_line_t l = ioapic_resolve(a, irq);
    unsigned i, p;
    if (find(a, l.gsi, &i, &p) != 0) return;
    uint32_t lo = rd(i, REG_REDIR(p));
    if (masked) lo |= 1u << 16; else lo &= ~(1u << 16);
    wr(i, REG_REDIR(p), lo);
}

unsigned ioapic_entry_count(void) { return count ? nent[0] : 0; }
#endif
