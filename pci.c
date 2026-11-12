#include "pci.h"

#if !defined(KIRA_HOST_TEST) && defined(__x86_64__)
#include "spinlock.h"
/* Порты 0xCF8/0xCFC - пара обращений: с драйверов Ring 3 (kdrv.c) конфигурацию читают и пишут разные ядра и потоки одновременно. */
static spinlock_t cfg_port_lock = SPINLOCK_INIT;
static inline void outl(uint16_t p, uint32_t v) { __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
#endif

/* Порты 0xCF8/0xCFC - только у x86; на других архитектурах конфигурация - через ECAM (pci_use_ecam). */
static uint32_t port_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
#if !defined(KIRA_HOST_TEST) && defined(__x86_64__)
    uint64_t f = spin_lock_irqsave(&cfg_port_lock);
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC));
    uint32_t v = inl(0xCFC);
    spin_unlock_irqrestore(&cfg_port_lock, f);
    return v;
#else
    (void)bus; (void)dev; (void)fn; (void)off; return 0xFFFFFFFFu;
#endif
}
static void port_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint32_t v) {
#if !defined(KIRA_HOST_TEST) && defined(__x86_64__)
    uint64_t f = spin_lock_irqsave(&cfg_port_lock);
    outl(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)fn << 8) | (off & 0xFC));
    outl(0xCFC, v);
    spin_unlock_irqrestore(&cfg_port_lock, f);
#else
    (void)bus; (void)dev; (void)fn; (void)off; (void)v;
#endif
}

static pci_backend_t be = { port_read32, port_write32 };
static pci_dev_t devs[PCI_MAX_DEVS];
static int ndevs;
static uint8_t bus_seen[256];

void pci_set_backend(const pci_backend_t* b) {
    if (b && b->read32 && b->write32) be = *b;
    else { be.read32 = port_read32; be.write32 = port_write32; }
}

/* ECAM (PCI Express): конфигурация каждой функции - 4 КБ по адресу base + (шина << 20 | устройство << 15 |
   функция << 12). Так её видят ARM-платы (дерево устройств: pci-host-ecam-generic) и x86 с таблицей MCFG. */
static volatile uint8_t* ecam;
static uint64_t ecam_size;
static uint8_t ecam_bus0, ecam_bus1;
static uint32_t ecam_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off) {
    if (!ecam || bus < ecam_bus0 || bus > ecam_bus1 || dev >= 32 || fn >= 8 || off >= 4096)
        return 0xFFFFFFFFu;
    uint64_t offset = ((uint64_t)(bus - ecam_bus0) << 20) |
                      ((uint64_t)dev << 15) | ((uint64_t)fn << 12) | (off & 0xFFCu);
    if (offset > ecam_size || ecam_size - offset < sizeof(uint32_t)) return 0xFFFFFFFFu;
    return *(volatile uint32_t*)(ecam + offset);
}
static void ecam_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off, uint32_t v) {
    if (!ecam || bus < ecam_bus0 || bus > ecam_bus1 || dev >= 32 || fn >= 8 || off >= 4096)
        return;
    uint64_t offset = ((uint64_t)(bus - ecam_bus0) << 20) |
                      ((uint64_t)dev << 15) | ((uint64_t)fn << 12) | (off & 0xFFCu);
    if (offset > ecam_size || ecam_size - offset < sizeof(uint32_t)) return;
    *(volatile uint32_t*)(ecam + offset) = v;
}
int pci_use_ecam(volatile void* base, uint64_t size, uint8_t bus_start, uint8_t bus_end) {
    if (!base || ((uintptr_t)base & 3u) || !size || bus_start > bus_end || size > (uint64_t)UINTPTR_MAX ||
        (uintptr_t)base > UINTPTR_MAX - (uintptr_t)size) return -1;
    uint64_t required = ((uint64_t)bus_end - bus_start + 1) << 20;
    if (size < required) return -1;
    ecam = (volatile uint8_t*)base;
    ecam_size = size;
    ecam_bus0 = bus_start;
    ecam_bus1 = bus_end;
    be.read32 = ecam_read32;
    be.write32 = ecam_write32;
    return 0;
}

/* Назначение адресов BAR, когда прошивка этого не сделала (ARM без UEFI: ядро загружено напрямую). Окна - из
   дерева устройств (ranges моста). Каждый BAR выровнен по своему размеру; 64-битные - в 64-битное окно, если оно
   есть. cpu_off - разница «адрес процессора минус адрес шины» окна (у QEMU virt - 0). Возвращает число BAR. */
int pci_window_alloc(pci_window_t* w, uint64_t size, uint64_t* pci_addr, uint64_t* cpu_addr) {
    if (!w || !pci_addr || !cpu_addr || !size || (size & (size - 1)) || !w->size ||
        w->pci_base > UINT64_MAX - w->size) return -1;
    uint64_t end = w->pci_base + w->size;
    uint64_t start = w->next;
    if (start < w->pci_base || start >= end || start > UINT64_MAX - (size - 1)) return -1;
    uint64_t a = (start + size - 1) & ~(size - 1);
    if (a < start || a > end || size > end - a) return -1;

    uint64_t cpu;
    if (w->cpu_off >= 0) {
        uint64_t off = (uint64_t)w->cpu_off;
        if (a > UINT64_MAX - off) return -1;
        cpu = a + off;
    } else {
        uint64_t off = (uint64_t)(-(w->cpu_off + 1)) + 1;
        if (a < off) return -1;
        cpu = a - off;
    }
    w->next = a + size;
    *pci_addr = a;
    *cpu_addr = cpu;
    return 0;
}
static int pci_window_cpu_range(const pci_window_t* w, uint64_t* start, uint64_t* end) {
    uint64_t base;
    if (w->cpu_off >= 0) {
        uint64_t off = (uint64_t)w->cpu_off;
        if (w->pci_base > UINT64_MAX - off) return -1;
        base = w->pci_base + off;
    } else {
        uint64_t off = (uint64_t)(-(w->cpu_off + 1)) + 1;
        if (w->pci_base < off) return -1;
        base = w->pci_base - off;
    }
    if (base > UINT64_MAX - w->size) return -1;
    *start = base; *end = base + w->size;
    return 0;
}
int pci_window_set_add(pci_window_set_t* set, const pci_window_t* window) {
    if (!set || !window || !window->size || window->pci_base > UINT64_MAX - window->size ||
        window->next < window->pci_base || window->next > window->pci_base + window->size ||
        set->count >= PCI_MAX_WINDOWS) return -1;
    uint64_t end = window->pci_base + window->size, cpu_start, cpu_end;
    if (pci_window_cpu_range(window, &cpu_start, &cpu_end)) return -1;
    for (unsigned i = 0; i < set->count; i++) {
        const pci_window_t* old = &set->windows[i];
        uint64_t old_end = old->pci_base + old->size, old_cpu_start, old_cpu_end;
        if (window->pci_base < old_end && old->pci_base < end) return -1;
        if (pci_window_cpu_range(old, &old_cpu_start, &old_cpu_end) ||
            (cpu_start < old_cpu_end && old_cpu_start < cpu_end)) return -1;
    }
    set->windows[set->count++] = *window;
    return 0;
}
int pci_window_set_alloc(pci_window_set_t* set, uint64_t size, uint64_t* pci_addr, uint64_t* cpu_addr) {
    if (!set || !pci_addr || !cpu_addr) return -1;
    for (unsigned i = 0; i < set->count; i++) {
        if (pci_window_alloc(&set->windows[i], size, pci_addr, cpu_addr) == 0) return 0;
    }
    return -1;
}
static int pci_window_set_alloc_limit(pci_window_set_t* set, uint64_t size, uint64_t max_addr,
                                         uint64_t* pci_addr, uint64_t* cpu_addr) {
    if (!set || !pci_addr || !cpu_addr || !size) return -1;
    for (unsigned i = 0; i < set->count; i++) {
        pci_window_t candidate = set->windows[i];
        uint64_t a = 0, cpu = 0;
        if (pci_window_alloc(&candidate, size, &a, &cpu) == 0 &&
            a <= max_addr && size - 1 <= max_addr - a) {
            set->windows[i] = candidate;
            *pci_addr = a;
            *cpu_addr = cpu;
            return 0;
        }
    }
    return -1;
}

/* MEM32 and MEM64 are separate allocation pools, but PCI memory addresses and
   their CPU mappings share one physical address space. Reject aliases across
   pools before assigning any BAR, otherwise devices can receive the same
   effective address through different host-bridge windows. */
static int pci_window_sets_disjoint(const pci_window_set_t* a, const pci_window_set_t* b) {
    if (!a || !b) return 1;
    if (a->count > PCI_MAX_WINDOWS || b->count > PCI_MAX_WINDOWS) return 0;
    for (unsigned i = 0; i < a->count; i++) {
        const pci_window_t* wa = &a->windows[i];
        uint64_t a_cpu_start, a_cpu_end;
        if (!wa->size || wa->pci_base > UINT64_MAX - wa->size ||
            wa->next < wa->pci_base || wa->next > wa->pci_base + wa->size ||
            pci_window_cpu_range(wa, &a_cpu_start, &a_cpu_end)) return 0;
        uint64_t a_pci_end = wa->pci_base + wa->size;
        for (unsigned j = 0; j < b->count; j++) {
            const pci_window_t* wb = &b->windows[j];
            uint64_t b_cpu_start, b_cpu_end;
            if (!wb->size || wb->pci_base > UINT64_MAX - wb->size ||
                wb->next < wb->pci_base || wb->next > wb->pci_base + wb->size ||
                pci_window_cpu_range(wb, &b_cpu_start, &b_cpu_end)) return 0;
            uint64_t b_pci_end = wb->pci_base + wb->size;
            if ((wa->pci_base < b_pci_end && wb->pci_base < a_pci_end) ||
                (a_cpu_start < b_cpu_end && b_cpu_start < a_cpu_end)) return 0;
        }
    }
    return 1;
}

int pci_assign_multi(pci_window_set_t* mem32, pci_window_set_t* mem64) {
    if (!pci_window_sets_disjoint(mem32, mem64)) return 0;
    int n = 0;
    for (int pass = 63; pass >= 4; pass--) {
        for (int i = 0; i < ndevs; i++) {
            pci_dev_t* d = &devs[i];
            for (int b = 0; b < PCI_BARS; b++) {
                uint64_t sz = d->bar[b].size;
                if (d->bar[b].io || !sz || d->bar[b].base || (sz & (sz - 1)) || sz != (1ull << pass)) continue;
                uint64_t a = 0, cpu = 0;
                int rc = -1;
                if (d->bar[b].mem64 && mem64)
                    rc = pci_window_set_alloc_limit(mem64, sz, UINT64_MAX, &a, &cpu);
                if (rc && mem32)
                    rc = pci_window_set_alloc_limit(mem32, sz, UINT32_MAX, &a, &cpu);
                if (rc) continue;
                uint16_t off = (uint16_t)(0x10 + 4 * b);
                uint32_t lo = pci_read32(d, off) & 0xF;
                pci_write32(d, off, (uint32_t)a | lo);
                if (d->bar[b].mem64) pci_write32(d, (uint16_t)(off + 4), (uint32_t)(a >> 32));
                d->bar[b].base = cpu;
                n++;
            }
        }
    }
    return n;
}
int pci_assign(pci_window_t* mem32, pci_window_t* mem64) {
    pci_window_set_t s32 = {0}, s64 = {0};
    if (mem32 && mem32->size && pci_window_set_add(&s32, mem32)) return 0;
    if (mem64 && mem64->size && pci_window_set_add(&s64, mem64)) return 0;
    return pci_assign_multi(&s32, &s64);
}

static uint32_t cr32(const pci_dev_t* d, uint16_t off) { return be.read32(d->bus, d->dev, d->fn, off); }
uint32_t pci_read32(const pci_dev_t* d, uint16_t off) { return cr32(d, off); }
uint16_t pci_read16(const pci_dev_t* d, uint16_t off) { return (uint16_t)(cr32(d, off & ~3) >> ((off & 2) * 8)); }
uint8_t pci_read8(const pci_dev_t* d, uint16_t off) { return (uint8_t)(cr32(d, off & ~3) >> ((off & 3) * 8)); }
void pci_write32(const pci_dev_t* d, uint16_t off, uint32_t v) { be.write32(d->bus, d->dev, d->fn, off, v); }
void pci_write16(const pci_dev_t* d, uint16_t off, uint16_t v) {
    uint32_t w = cr32(d, off & ~3), sh = (off & 2) * 8;
    if ((off & ~3) == 0x04) w &= 0x0000FFFFu;                        /* старшая половина - статус с битами «сбросить записью 1» */
    w = (w & ~(0xFFFFu << sh)) | ((uint32_t)v << sh);
    be.write32(d->bus, d->dev, d->fn, off & ~3, w);
}

void pci_enable(const pci_dev_t* d, uint16_t bits) {
    pci_write16(d, 0x04, (uint16_t)(pci_read16(d, 0x04) | bits));
}

/* Размер и тип BAR: пишем единицы, читаем маску, возвращаем исходное значение. Декодирование на время зонда выключено. */
static void probe_bars(pci_dev_t* d) {
    uint16_t cmd = pci_read16(d, 0x04);
    pci_write16(d, 0x04, (uint16_t)(cmd & ~(PCI_CMD_IO | PCI_CMD_MEM)));
    int nb = (d->header_type & 0x7F) == 0 ? 6 : ((d->header_type & 0x7F) == 1 ? 2 : 0);
    for (int i = 0; i < PCI_BARS; i++) { d->bar[i].base = d->bar[i].size = 0; d->bar[i].io = d->bar[i].mem64 = d->bar[i].prefetch = 0; }
    for (int i = 0; i < nb; i++) {
        uint16_t off = (uint16_t)(0x10 + 4 * i);
        uint32_t orig = pci_read32(d, off);
        pci_write32(d, off, 0xFFFFFFFFu);
        uint32_t mask = pci_read32(d, off);
        pci_write32(d, off, orig);
        if (mask == 0 || mask == 0xFFFFFFFFu) continue;
        if (orig & 1) {                                           /* порты ввода-вывода */
            uint32_t m = (mask & ~3u) | 0xFFFF0000u;                  /* порты - 16 бит: старшие биты маски не значимы */
            d->bar[i].io = 1; d->bar[i].base = orig & ~3u;
            d->bar[i].size = ~m + 1;
            continue;
        }
        int is64 = ((orig >> 1) & 3) == 2;
        d->bar[i].prefetch = (orig >> 3) & 1;
        uint64_t base = orig & ~0xFull, m = mask & ~0xFull;
        if (is64 && i + 1 < nb) {
            uint16_t off2 = (uint16_t)(off + 4);
            uint32_t orig_hi = pci_read32(d, off2);
            pci_write32(d, off2, 0xFFFFFFFFu);
            uint32_t mask_hi = pci_read32(d, off2);
            pci_write32(d, off2, orig_hi);
            base |= (uint64_t)orig_hi << 32; m |= (uint64_t)mask_hi << 32;
            d->bar[i].mem64 = 1;
            d->bar[i].base = base; d->bar[i].size = ~m + 1;
            i++;                                                   /* BAR i+1 - старшая половина, самостоятельного BAR нет */
        } else {
            d->bar[i].base = base; d->bar[i].size = (uint32_t)(~(uint32_t)m + 1);
        }
    }
    pci_write16(d, 0x04, cmd);
}

static void scan_bus(uint8_t bus);

static void scan_fn(uint8_t bus, uint8_t dev, uint8_t fn) {
    pci_dev_t t; t.bus = bus; t.dev = dev; t.fn = fn;
    uint32_t id = cr32(&t, 0);
    if ((id & 0xFFFF) == 0xFFFF) return;
    if (ndevs >= PCI_MAX_DEVS) return;
    pci_dev_t* d = &devs[ndevs++];
    d->bus = bus; d->dev = dev; d->fn = fn;
    d->vendor = (uint16_t)id; d->device = (uint16_t)(id >> 16);
    uint32_t cls = cr32(d, 0x08);
    d->revision = (uint8_t)cls; d->prog_if = (uint8_t)(cls >> 8); d->subclass = (uint8_t)(cls >> 16); d->class_code = (uint8_t)(cls >> 24);
    d->header_type = pci_read8(d, 0x0E);
    probe_bars(d);
    if ((d->header_type & 0x7F) == 1 && d->class_code == 6 && d->subclass == 4) {
        uint8_t sec = pci_read8(d, 0x19);                          /* вторичная шина моста */
        if (sec) scan_bus(sec);
    }
}

static void scan_bus(uint8_t bus) {
    if (bus_seen[bus]) return;                                       /* защита от петель в неверно настроенных мостах */
    bus_seen[bus] = 1;
    for (uint8_t dev = 0; dev < 32; dev++) {
        pci_dev_t t; t.bus = bus; t.dev = dev; t.fn = 0;
        if ((cr32(&t, 0) & 0xFFFF) == 0xFFFF) continue;
        uint8_t multi = pci_read8(&t, 0x0E) & 0x80;
        scan_fn(bus, dev, 0);
        if (multi) for (uint8_t fn = 1; fn < 8; fn++) scan_fn(bus, dev, fn);
    }
}

int pci_init(void) {
    ndevs = 0;
    for (int i = 0; i < 256; i++) bus_seen[i] = 0;
    scan_bus(0);
    return ndevs;
}

int pci_count(void) { return ndevs; }
const pci_dev_t* pci_get(int i) { return i >= 0 && i < ndevs ? &devs[i] : 0; }
const pci_dev_t* pci_find(uint16_t v, uint16_t dv) {
    for (int i = 0; i < ndevs; i++) if (devs[i].vendor == v && devs[i].device == dv) return &devs[i];
    return 0;
}

uint16_t pci_next_cap(const pci_dev_t* d, uint16_t prev, uint8_t* id) {
    if (!d || !id || (prev && (prev < 0x40 || prev > 0xFC || (prev & 3)))) return 0;
    if (!(pci_read16(d, 0x06) & 0x10)) return 0;
    uint16_t raw = prev ? pci_read8(d, (uint16_t)(prev + 1)) : pci_read8(d, 0x34);
    uint16_t p = raw & 0xFC;
    if (p < 0x40 || p > 0xFC || p == prev) return 0;
    uint8_t cap_id = pci_read8(d, p);
    if (!cap_id || cap_id == 0xFF) return 0;
    *id = cap_id;
    return p;
}

/* A malformed capability list is not equivalent to "no MSI". Mask every
   discoverable message interrupt and legacy INTx, then verify config readback. */
int pci_quiesce_interrupts(const pci_dev_t* d) {
    if (!d) return 0;
    int safe = 1;
    uint16_t status = pci_read16(d, 0x06);
    if (status & 0x10u) {
        uint8_t ptr = pci_read8(d, 0x34);
        uint64_t visited = 0;
        if (!ptr) safe = 0;
        unsigned n;
        for (n = 0; ptr && n < 48; n++) {
            if (ptr < 0x40u || ptr > 0xFCu || (ptr & 3u)) { safe = 0; break; }
            uint64_t bit = 1ull << (ptr >> 2);
            if (visited & bit) { safe = 0; break; }
            visited |= bit;
            uint8_t id = pci_read8(d, ptr);
            uint8_t next = pci_read8(d, (uint16_t)(ptr + 1u));
            if (!id || id == 0xFFu) { safe = 0; break; }
            if (id == 0x05u) {
                uint16_t control = pci_read16(d, (uint16_t)(ptr + 2u));
                pci_write16(d, (uint16_t)(ptr + 2u), (uint16_t)(control & ~1u));
                if (pci_read16(d, (uint16_t)(ptr + 2u)) & 1u) safe = 0;
            } else if (id == 0x11u) {
                uint16_t control = pci_read16(d, (uint16_t)(ptr + 2u));
                pci_write16(d, (uint16_t)(ptr + 2u), (uint16_t)(control | 0x4000u));
                uint16_t after = pci_read16(d, (uint16_t)(ptr + 2u));
                if (!(after & 0x4000u)) safe = 0;
            }
            if (!next) { ptr = 0; break; }
            if (next < 0x40u || next > 0xFCu || (next & 3u) || next == ptr) {
                safe = 0;
                break;
            }
            ptr = next;
        }
        if (ptr && n == 48) safe = 0;
    }
    uint16_t command = pci_read16(d, 0x04);
    pci_write16(d, 0x04, (uint16_t)(command | (1u << 10)));
    if (!(pci_read16(d, 0x04) & (1u << 10))) safe = 0;
    return safe;
}

/* Отображение части BAR в окно MMIO ядра: без кэширования и без исполнения. */
#ifndef KIRA_HOST_TEST
#include "vmm.h"
volatile void* pci_map_bar(const pci_dev_t* d, int bar, uint64_t off, uint64_t len) {
    if (!d || bar < 0 || bar >= PCI_BARS || d->bar[bar].io || !d->bar[bar].base ||
        !len || off > d->bar[bar].size || len > d->bar[bar].size - off ||
        d->bar[bar].base > UINT64_MAX - off) return 0;
    uint64_t physical = d->bar[bar].base + off;
    if (physical > UINT64_MAX - len) return 0;
    return vmm_map_mmio(physical, len, VMM_MMIO_UC);   /* окно MMIO ядра: где бы ни лежал BAR */
}
#endif

#ifndef KIRA_HOST_TEST
#include "uart.h"
static void hex2(uint32_t v, int digits) {
    for (int i = digits - 1; i >= 0; i--) uart_putchar("0123456789abcdef"[(v >> (4 * i)) & 15]);
}
void pci_print(void) {
    uart_print("шина:устр.функ  производитель:прибор  класс\n");
    for (int i = 0; i < ndevs; i++) {
        const pci_dev_t* d = &devs[i];
        uart_print("  "); hex2(d->bus, 2); uart_putchar(':'); hex2(d->dev, 2); uart_putchar('.'); hex2(d->fn, 1);
        uart_print("      "); hex2(d->vendor, 4); uart_putchar(':'); hex2(d->device, 4);
        uart_print("      "); hex2(d->class_code, 2); uart_putchar('.'); hex2(d->subclass, 2);
        if (d->vendor == 0x1AF4) uart_print("  virtio");
        uart_print("\n");
    }
}
#endif
