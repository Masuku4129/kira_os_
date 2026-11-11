#include "ioapic.h"
#include "pic.h"
#include "apic.h"

#ifndef KIRA_HOST_TEST
#include "vmm.h"
#include "uart.h"
#endif

#define IOREGSEL 0x00
#define IOWIN    0x10
#define REG_VER  0x01
#define REG_REDIR(n) (0x10 + 2 * (n))
#define IOAPIC_MAX_ENTRIES 240u
#define IOAPIC_MASKED (1u << 16)

irq_line_t ioapic_resolve(const acpi_info_t* a, unsigned irq) {
    irq_line_t l = { irq, 0, 0 };           /* ISA по умолчанию: GSI = IRQ, active-high, edge */
    if (a && irq < 16 && a->iso_valid[irq]) {
        l.gsi = a->iso_gsi[irq];
        uint16_t f = a->iso_flags[irq];
        if ((f & 3u) == 3u) l.active_low = 1;
        if (((f >> 2) & 3u) == 3u) l.level = 1;
    }
    return l;
}

void ioapic_encode(uint8_t vector, int active_low, int level, int masked,
                   uint32_t dest, uint32_t* lo, uint32_t* hi) {
    if (!lo || !hi) return;
    uint32_t v = vector;                    /* fixed delivery, physical destination */
    if (active_low) v |= 1u << 13;
    if (level)      v |= 1u << 15;
    if (masked)     v |= IOAPIC_MASKED;
    *lo = v;
    *hi = (dest & 0xFFu) << 24;
}

#ifndef KIRA_HOST_TEST
static volatile uint32_t* base[ACPI_MAX_IOAPIC];
static unsigned nent[ACPI_MAX_IOAPIC];
static uint8_t programmed[ACPI_MAX_IOAPIC][IOAPIC_MAX_ENTRIES];
static uint8_t programmed_vector[ACPI_MAX_IOAPIC][IOAPIC_MAX_ENTRIES];
static unsigned count;
static int ready;

static uint32_t rd(unsigned i, uint32_t reg) {
    base[i][IOREGSEL / 4] = reg;
    return base[i][IOWIN / 4];
}

static void wr(unsigned i, uint32_t reg, uint32_t value) {
    base[i][IOREGSEL / 4] = reg;
    base[i][IOWIN / 4] = value;
}

int ioapic_init(const acpi_info_t* a) {
    count = 0;
    ready = 0;
    for (unsigned i = 0; i < ACPI_MAX_IOAPIC; ++i) {
        base[i] = 0;
        nent[i] = 0;
        for (unsigned j = 0; j < IOAPIC_MAX_ENTRIES; ++j) {
            programmed[i][j] = 0;
            programmed_vector[i][j] = 0;
        }
    }

    if (!a || !a->valid || !a->ioapic_count ||
        a->ioapic_count > ACPI_MAX_IOAPIC)
        return -1;

    for (unsigned i = 0; i < a->ioapic_count; ++i) {
        uint64_t pa = a->ioapic[i].addr;
        if (!pa || (pa & 0xFFFu))
            return -2;

        volatile void* m = vmm_map_mmio(pa, 0x20, VMM_MMIO_UC);
        if (!m) {
            uart_print("[KIRA IOAPIC] cannot map MMIO page.\n");
            return -3;
        }
        base[i] = (volatile uint32_t*)m;

        uint32_t ver = rd(i, REG_VER);
        unsigned maxr = (ver >> 16) & 0xFFu;
        if ((ver & 0xFFu) == 0 || (ver & 0xFFu) == 0xFFu ||
            maxr < 7 || maxr >= IOAPIC_MAX_ENTRIES) {
            uart_print("[KIRA IOAPIC] invalid version register: ");
            uart_print_hex(ver);
            uart_print("\n");
            return -4;
        }
        nent[i] = maxr + 1u;

        /* Перекрывающиеся GSI-диапазоны делают выбор контроллера неоднозначным. */
        for (unsigned j = 0; j < i; ++j) {
            uint64_t a0 = a->ioapic[i].gsi_base;
            uint64_t a1 = a0 + nent[i];
            uint64_t b0 = a->ioapic[j].gsi_base;
            uint64_t b1 = b0 + nent[j];
            if (a0 < b1 && b0 < a1)
                return -5;
        }

        /* Не наследовать случайные/оставшиеся от прошивки маршруты. */
        for (unsigned pin = 0; pin < nent[i]; ++pin) {
            uint32_t low = rd(i, REG_REDIR(pin));
            wr(i, REG_REDIR(pin), low | IOAPIC_MASKED);
        }
        count = i + 1u;

        uart_print("[KIRA IOAPIC] id ");
        uart_print_dec(a->ioapic[i].id);
        uart_print(" at ");
        uart_print_hex(pa);
        uart_print(", version ");
        uart_print_hex(ver & 0xFFu);
        uart_print(", ");
        uart_print_dec(nent[i]);
        uart_print(" pins, GSI base ");
        uart_print_dec(a->ioapic[i].gsi_base);
        uart_print("\n");
    }

    ready = count != 0;
    return ready ? 0 : -6;
}

static int find_gsi(const acpi_info_t* a, uint32_t gsi,
                    unsigned* idx, unsigned* pin) {
    if (!a || !idx || !pin || !ready)
        return -1;
    for (unsigned i = 0; i < count && i < a->ioapic_count; ++i) {
        uint64_t first = a->ioapic[i].gsi_base;
        uint64_t end = first + nent[i];
        if ((uint64_t)gsi >= first && (uint64_t)gsi < end) {
            *idx = i;
            *pin = (unsigned)((uint64_t)gsi - first);
            return 0;
        }
    }
    return -1;
}

static int decode_irq_flags(uint16_t flags, int* active_low, int* level) {
    unsigned polarity = flags & 3u;
    unsigned trigger = (flags >> 2) & 3u;
    if (polarity == 2u || trigger == 2u)
        return -1;                           /* reserved ACPI encoding */
    *active_low = polarity == 3u;
    *level = trigger == 3u;
    return 0;
}

static int route_gsi_raw(const acpi_info_t* a, uint32_t gsi, uint8_t vector,
                         uint16_t flags, uint32_t dest) {
    unsigned i, pin;
    int active_low, level;
    if (!apic_is_enabled() || dest > 255u ||
        !((vector >= 32u && vector < 48u) ||
          (vector >= IRQ_VECTOR_BASE && vector < IRQ_VECTOR_BASE + IRQ_VECTOR_COUNT)) ||
        decode_irq_flags(flags, &active_low, &level) != 0 ||
        find_gsi(a, gsi, &i, &pin) != 0)
        return -1;

    if (programmed[i][pin] && programmed_vector[i][pin] != vector)
        return -2;                           /* один физический GSI — один вектор */

    uint32_t low, high;
    ioapic_encode(vector, active_low, level, 1, dest, &low, &high);
    wr(i, REG_REDIR(pin) + 1u, high);
    wr(i, REG_REDIR(pin), low);
    if ((rd(i, REG_REDIR(pin)) & ~0x5000u) != (low & ~0x5000u) ||
        rd(i, REG_REDIR(pin) + 1u) != high)
        return -3;                           /* запись не подтверждена; линия остаётся masked */

    programmed[i][pin] = 1;
    programmed_vector[i][pin] = vector;
    low &= ~IOAPIC_MASKED;
    wr(i, REG_REDIR(pin), low);
    if ((rd(i, REG_REDIR(pin)) & ~0x5000u) != (low & ~0x5000u)) {
        wr(i, REG_REDIR(pin), low | IOAPIC_MASKED);
        return -4;
    }
    return 0;
}

static int mask_gsi_raw(const acpi_info_t* a, uint32_t gsi, int masked) {
    unsigned i, pin;
    if (find_gsi(a, gsi, &i, &pin) != 0 || !programmed[i][pin])
        return -1;
    uint32_t low = rd(i, REG_REDIR(pin));
    if (masked) low |= IOAPIC_MASKED;
    else low &= ~IOAPIC_MASKED;
    wr(i, REG_REDIR(pin), low);
    if ((rd(i, REG_REDIR(pin)) & IOAPIC_MASKED) != (low & IOAPIC_MASKED))
        return -2;
    return 0;
}

int ioapic_route_gsi(const acpi_info_t* a, uint32_t gsi, uint8_t vector,
                     uint16_t flags, uint32_t dest) {
    if (vector < IRQ_VECTOR_BASE ||
        vector >= IRQ_VECTOR_BASE + IRQ_VECTOR_COUNT ||
        !irq_vector_is_allocated(vector))
        return -1;
    return route_gsi_raw(a, gsi, vector, flags, dest);
}

int ioapic_mask_gsi(const acpi_info_t* a, uint32_t gsi, int masked) {
    return mask_gsi_raw(a, gsi, masked);
}

int ioapic_route_isa(const acpi_info_t* a, unsigned irq, uint32_t dest) {
    if (!a || irq >= 16)
        return -1;
    uint16_t flags = a->iso_valid[irq] ? a->iso_flags[irq] : 0;
    irq_line_t line = ioapic_resolve(a, irq);
    return route_gsi_raw(a, line.gsi, (uint8_t)(32u + irq), flags, dest);
}

void ioapic_mask_isa(const acpi_info_t* a, unsigned irq, int masked) {
    if (!a || irq >= 16)
        return;
    irq_line_t line = ioapic_resolve(a, irq);
    (void)mask_gsi_raw(a, line.gsi, masked);
}

unsigned ioapic_entry_count(void) {
    return count ? nent[0] : 0;
}
#endif
