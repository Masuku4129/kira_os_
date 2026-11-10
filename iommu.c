/* DMA isolation through Intel VT-d: kernel glue between the DMAR parser (iommu_vtd.c),
   the register driver (drivers/iommu/intel_vtd.c) and the per-device domains
   (drivers/iommu/vtd_domain.c). See iommu.h for the model.

   Locking: G.lock (ticket spinlock) guards every table below. Inside it the code calls
   the page allocator and the VMM; nothing takes G.lock while holding those locks.
   Nothing is printed under G.lock. */
#include "iommu.h"
#include "iommu_vtd.h"
#include "drivers/iommu/intel_vtd.h"
#include "drivers/iommu/vtd_domain.h"
#include "acpi.h"
#include "uart.h"
#include "spinlock.h"
#ifndef KIRA_HOST_TEST
#include "pmm.h"
#include "vmm.h"
#include "ramext.h"
#endif

#define IOMMU_MAX_DOMAINS 128
#define IOMMU_MAX_MAPS 2048
#define IOMMU_FAULT_BATCH 32
#define IOMMU_FAULT_LOG_PER_DEVICE 8
#define IOMMU_AW_BITS 48

typedef struct { uint64_t iova; uint32_t pages; uint32_t rw; } imap_t;

typedef struct {
    uint8_t used, bus, dev, fn, unit;
    uint8_t blocked;                    /* uncertain IOTLB state: deny future DMA */
    struct kira_vtd_domain dom;
    imap_t maps[IOMMU_MAX_MAPS];
    uint32_t nmaps;
    uint32_t faults, logged;
} idom_t;

static struct {
    int active;                              /* at least one unit translates */
    uint32_t haw;                            /* physical address width of the host, bits */
    vtd_dmar_info_t info;
    struct kira_vtd_unit unit[VTD_MAX_UNITS];
    uint8_t usable[VTD_MAX_UNITS];           /* prepared and able to run pass-through + 4-level */
    uint8_t on[VTD_MAX_UNITS];               /* translation enabled */
    struct kira_vtd_platform plat;
    idom_t dom[IOMMU_MAX_DOMAINS];
    struct kira_vtd_fault batch[IOMMU_FAULT_BATCH];
    uint32_t nbatch, dropped;
    uint32_t faults_total;
    spinlock_t lock;
    volatile int polling;
} G;                                          /* zero state equals SPINLOCK_INIT */

/* ---- Platform: how the register driver reaches memory ---- */

#ifdef KIRA_HOST_TEST
extern volatile void* host_iommu_map_regs(uint64_t phys, uint64_t len);
extern void* host_iommu_page_alloc(uint64_t* phys);
extern void host_iommu_page_free(void* page, uint64_t phys);
extern const void* host_iommu_dmar(uint64_t phys, uint64_t len);
#define PLAT_MAP_REGS(p, l) host_iommu_map_regs((p), (l))
#define PLAT_DMAR(p, l) host_iommu_dmar((p), (l))
static void* plat_alloc(void* c, uint64_t* phys) { (void)c; return host_iommu_page_alloc(phys); }
static void plat_free(void* c, void* v, uint64_t phys) { (void)c; host_iommu_page_free(v, phys); }
static void plat_clean(void* c, const void* p, size_t n) { (void)c; (void)p; (void)n; }
#else
#define PLAT_MAP_REGS(p, l) vmm_map_mmio((p), (l), VMM_MMIO_UC)
static const void* dmar_view(uint64_t phys, uint64_t len) {
    if (ramext_map_read(phys, len)) return (const void*)(uintptr_t)phys;
    return (const void*)vmm_map_mmio(phys, len, VMM_MMIO_UC);
}
#define PLAT_DMAR(p, l) dmar_view((p), (l))
/* A table page must be reachable by the CPU at the very address the hardware walks. */
static void* plat_alloc(void* c, uint64_t* phys) {
    (void)c;
    uint64_t pa = pmm_alloc_page(), t;
    *phys = 0;
    if (!pa) return 0;
    if (vmm_translate(pa, &t) != 0 || t != pa) { pmm_free_page(pa); return 0; }
    *phys = pa;
    return (void*)(uintptr_t)pa;
}
static void plat_free(void* c, void* v, uint64_t phys) { (void)c; (void)v; pmm_free_page(phys); }
/* Table walks of a unit without ECAP.C do not snoop the CPU caches: write the lines back. Flushing is
   harmless on a coherent unit, so it is done for every unit. */
static void plat_clean(void* c, const void* p, size_t n) {
    (void)c;
    const volatile uint8_t* a = (const volatile uint8_t*)p;
    for (size_t i = 0; i < n; i += 64) __asm__ volatile ("clflush (%0)" : : "r"(a + i) : "memory");
    __asm__ volatile ("mfence" : : : "memory");
}
#endif

static volatile void* plat_map_mmio(void* c, uint64_t phys, size_t len) { (void)c; return PLAT_MAP_REGS(phys, len); }
static void* plat_phys_to_virt(void* c, uint64_t phys) { (void)c; return (void*)(uintptr_t)phys; }
#ifdef KIRA_HOST_TEST
extern void host_iommu_barrier(void);             /* the hardware model reacts to register writes here */
static void plat_barrier(void* c) { (void)c; __sync_synchronize(); host_iommu_barrier(); }
#else
static void plat_barrier(void* c) { (void)c; __sync_synchronize(); }
#endif

static void put_hex(uint64_t v, int digits) {
    char b[17];
    for (int i = 0; i < digits; i++) b[i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 15];
    b[digits] = 0;
    uart_print(b);
}
static void print_bdf(uint8_t bus, uint8_t dev, uint8_t fn) {
    put_hex(bus, 2); uart_print(":"); put_hex(dev, 2); uart_print("."); put_hex(fn, 1);
}
static void print_err(int rc) { uart_print("code "); uart_print_dec((uint64_t)(rc < 0 ? -rc : rc)); }

/* Bridge topology for DMAR device-scope paths, taken from PCI configuration space. */
static int bridge_buses(void* ctx, uint8_t bus, uint8_t dev, uint8_t fn, uint8_t* sec, uint8_t* sub) {
    (void)ctx;
    for (int i = 0; i < pci_count(); i++) {
        const pci_dev_t* d = pci_get(i);
        if (d->bus != bus || d->dev != dev || d->fn != fn) continue;
        if ((d->header_type & 0x7F) != 1) return -1;
        *sec = pci_read8(d, 0x19); *sub = pci_read8(d, 0x1A);
        return 0;
    }
    return -1;
}

static int unit_of(uint8_t bus, uint8_t dev, uint8_t fn) {
    return vtd_dmar_unit_of(&G.info, 0, bus, dev, fn, bridge_buses, 0);
}
static int rmrr_of(uint8_t bus, uint8_t dev, uint8_t fn) {
    return vtd_dmar_rmrr_of(&G.info, 0, bus, dev, fn, bridge_buses, 0);
}

static idom_t* dom_find(uint8_t bus, uint8_t dev, uint8_t fn) {
    for (int i = 0; i < IOMMU_MAX_DOMAINS; i++)
        if (G.dom[i].used && G.dom[i].bus == bus && G.dom[i].dev == dev && G.dom[i].fn == fn) return &G.dom[i];
    return 0;
}

/* ---- Initialisation ---- */

int iommu_enabled(void) {
    int n = 0;
    for (int i = 0; i < VTD_MAX_UNITS; i++) n += G.on[i];
    return n;
}

static void zero_bytes(void* p, size_t n) { uint8_t* b = (uint8_t*)p; while (n--) *b++ = 0; }

int iommu_init(void) {
    if (G.active) return iommu_enabled();
    const acpi_info_t* ai = acpi_get();
    if (!ai || !ai->dmar_phys) {
        uart_print("[KIRA IOMMU] No DMAR table: DMA isolation is not available, devices keep unrestricted DMA.\n");
        return 0;
    }
    const void* table = PLAT_DMAR(ai->dmar_phys, ai->dmar_len);
    if (!table) {
        uart_print("[KIRA IOMMU] DMAR table cannot be read: VT-d left off, devices keep unrestricted DMA.\n");
        return -1;
    }
    int rc = vtd_dmar_parse(table, ai->dmar_len, &G.info);
    if (rc != 0) {
        zero_bytes(&G.info, sizeof G.info);
        uart_print("[KIRA IOMMU] DMAR table rejected ("); print_err(rc);
        uart_print("): VT-d left off, devices keep unrestricted DMA.\n");
        return -1;
    }
    G.haw = (uint32_t)G.info.host_address_width + 1u;
    G.plat.context = 0;
    G.plat.map_mmio = plat_map_mmio;
    G.plat.alloc_dma_page = plat_alloc;
    G.plat.free_dma_page = plat_free;
    G.plat.write_barrier = plat_barrier;
    G.plat.read_barrier = plat_barrier;
    G.plat.clean_cache = plat_clean;
    G.plat.phys_to_virt = plat_phys_to_virt;

    uart_print("[KIRA IOMMU] DMAR: "); uart_print_dec(G.info.unit_count); uart_print(" remapping unit(s), ");
    uart_print_dec(G.info.rmrr_count); uart_print(" reserved region(s), host address width ");
    uart_print_dec(G.haw); uart_print(" bits.\n");

    for (uint32_t i = 0; i < G.info.unit_count; i++) {
        const vtd_unit_t* u = &G.info.units[i];
        uart_print("[KIRA IOMMU] unit "); uart_print_dec(i); uart_print(" regs "); uart_print_hex(u->register_base);
        if (u->segment != 0) { uart_print(": PCI segment "); uart_print_dec(u->segment); uart_print(" is not handled, devices there stay unrestricted\n"); continue; }
        rc = kira_vtd_unit_prepare(&G.unit[i], u->register_base, u->register_bytes, IOMMU_AW_BITS, &G.plat);
        if (rc != KIRA_VTD_OK) {
            uart_print(": cannot be prepared ("); print_err(rc); uart_print("), left off, its devices stay unrestricted\n");
            continue;
        }
        if (!(G.unit[i].extended_capabilities & (1ull << 6))) {
            uart_print(": no pass-through support, left off, its devices stay unrestricted\n");
            continue;
        }
        G.usable[i] = 1;
        uart_print(u->include_all ? ": covers all devices of the segment\n" : ": covers the devices of its scope\n");
    }

    /* Every enumerated function gets an explicit context before translation is turned on:
       a function without one would be blocked by the hardware. */
    uint8_t failed[VTD_MAX_UNITS];
    zero_bytes(failed, sizeof failed);
    uint32_t n_pass = 0, n_uncovered = 0, n_rmrr = 0;
    for (int k = 0; k < pci_count(); k++) {
        const pci_dev_t* d = pci_get(k);
        int ui = unit_of(d->bus, d->dev, d->fn);
        uart_print("[KIRA IOMMU] "); print_bdf(d->bus, d->dev, d->fn); uart_print(" ");
        put_hex(d->vendor, 4); uart_print(":"); put_hex(d->device, 4);
        if (ui < 0) { uart_print(" no DMAR unit covers it: not translated, DMA unrestricted\n"); n_uncovered++; continue; }
        if (!G.usable[ui]) { uart_print(" its DMAR unit is not active: DMA unrestricted\n"); n_uncovered++; continue; }
        rc = kira_vtd_context_passthrough(&G.unit[ui], 0, d->bus, d->dev, d->fn, &G.plat);
        if (rc != KIRA_VTD_OK) {
            failed[ui] = 1;
            uart_print(" pass-through context failed ("); print_err(rc); uart_print("): unit "); uart_print_dec((uint64_t)ui);
            uart_print(" will not be enabled\n");
            continue;
        }
        int r = rmrr_of(d->bus, d->dev, d->fn);
        uart_print(" passthrough: unrestricted DMA, NOT isolated");
        if (r >= 0) { uart_print(" (RMRR "); uart_print_hex(G.info.rmrr[r].base); uart_print("-"); uart_print_hex(G.info.rmrr[r].limit); uart_print(": firmware DMA region, cannot be isolated)"); n_rmrr++; }
        uart_print("\n");
        n_pass++;
        /* A conventional PCI/PCI-X segment behind a bridge reaches the IOMMU with the
           requester id (secondary bus, 0, 0); give that id the same pass-through. */
        if ((d->header_type & 0x7F) == 1 && d->class_code == 6 && d->subclass == 4) {
            uint8_t sec = pci_read8(d, 0x19);
            int own = 0;
            for (int j = 0; sec && j < pci_count(); j++) { const pci_dev_t* e = pci_get(j); if (e->bus == sec && e->dev == 0 && e->fn == 0) own = 1; }
            if (sec && !own && unit_of(sec, 0, 0) == ui) {
                rc = kira_vtd_context_passthrough(&G.unit[ui], 0, sec, 0, 0, &G.plat);
                if (rc != KIRA_VTD_OK) failed[ui] = 1;
            }
        }
    }

    for (uint32_t i = 0; i < G.info.unit_count; i++) {
        if (!G.usable[i]) continue;
        if (failed[i]) { uart_print("[KIRA IOMMU] unit "); uart_print_dec(i); uart_print(" left off: a context could not be built\n"); continue; }
        rc = kira_vtd_unit_enable_translation(&G.unit[i], &G.plat);
        if (rc != KIRA_VTD_OK) {
            uart_print("[KIRA IOMMU] unit "); uart_print_dec(i); uart_print(" translation did not start ("); print_err(rc); uart_print(")\n");
            (void)kira_vtd_unit_disable_translation(&G.unit[i], &G.plat);
            continue;
        }
        G.on[i] = 1;
    }
    G.active = iommu_enabled() > 0;
    uart_print("[KIRA IOMMU] VT-d translation enabled on "); uart_print_dec((uint64_t)iommu_enabled());
    uart_print(" unit(s): "); uart_print_dec(n_pass); uart_print(" device(s) passthrough (unrestricted DMA, ");
    uart_print_dec(n_rmrr); uart_print(" of them RMRR), "); uart_print_dec(n_uncovered);
    uart_print(" without translation, 0 isolated until a driver asks.\n");
    return iommu_enabled();
}

/* ---- Domains ---- */

int iommu_device_isolated(const pci_dev_t* d) {
    if (!d || !G.active) return 0;
    uint64_t f = spin_lock_irqsave(&G.lock);
    idom_t* x = dom_find(d->bus, d->dev, d->fn);
    int r = x && !x->blocked && x->dom.active && G.on[x->unit];
    spin_unlock_irqrestore(&G.lock, f);
    return r;
}

int iommu_device_ready(const pci_dev_t* d) {
    if (!d) return 0;
    if (!G.active) return 1;
    int ui = unit_of(d->bus, d->dev, d->fn);
    if (ui < 0 || ui >= VTD_MAX_UNITS || !G.on[ui]) return 1;
    if (rmrr_of(d->bus, d->dev, d->fn) >= 0) return 0;
    uint64_t f = spin_lock_irqsave(&G.lock);
    idom_t* x = dom_find(d->bus, d->dev, d->fn);
    int ready = x && !x->blocked && x->unit == (uint8_t)ui && x->dom.active && G.on[x->unit];
    spin_unlock_irqrestore(&G.lock, f);
    return ready;
}

int iommu_isolate_device(const pci_dev_t* d) {
    if (!d) return IOMMU_E_ARG;
    if (!G.active) return IOMMU_E_NONE;
    int ui = unit_of(d->bus, d->dev, d->fn);
    int rc = IOMMU_OK, rm = -1;
    uint64_t f = spin_lock_irqsave(&G.lock);
    if (dom_find(d->bus, d->dev, d->fn)) { spin_unlock_irqrestore(&G.lock, f); return IOMMU_OK; }
    if (ui < 0 || !G.on[ui]) rc = IOMMU_E_NOCOVER;
    else if ((rm = rmrr_of(d->bus, d->dev, d->fn)) >= 0) rc = IOMMU_E_RMRR;
    else {
        idom_t* x = 0;
        for (int i = 0; i < IOMMU_MAX_DOMAINS && !x; i++) if (!G.dom[i].used) x = &G.dom[i];
        if (!x) rc = IOMMU_E_EXIST;
        else {
            zero_bytes(x, sizeof *x);
            int hr = kira_vtd_domain_create(&x->dom, &G.unit[ui], 0, d->bus, d->dev, d->fn, &G.plat);
            if (hr == KIRA_VTD_OK || x->dom.active) {      /* the domain is live even if the final flush failed */
                x->used = 1; x->bus = d->bus; x->dev = d->dev; x->fn = d->fn; x->unit = (uint8_t)ui;
                rc = hr == KIRA_VTD_OK ? IOMMU_OK : IOMMU_E_HW;
            } else rc = hr == KIRA_VTD_MEMORY_FAILED ? IOMMU_E_NOMEM : IOMMU_E_HW;
        }
    }
    spin_unlock_irqrestore(&G.lock, f);
    uart_print("[KIRA IOMMU] "); print_bdf(d->bus, d->dev, d->fn);
    if (rc == IOMMU_OK) uart_print(" isolated: own translation domain, no DMA allowed until buffers are mapped\n");
    else if (rc == IOMMU_E_RMRR) { uart_print(" NOT isolated: named by RMRR "); uart_print_hex(G.info.rmrr[rm].base); uart_print("-"); uart_print_hex(G.info.rmrr[rm].limit); uart_print(", stays passthrough\n"); }
    else if (rc == IOMMU_E_NOCOVER) uart_print(" NOT isolated: no active DMAR unit covers it, stays unrestricted\n");
    else { uart_print(" NOT isolated: domain setup failed ("); print_err(rc); uart_print("), stays passthrough\n"); }
    return rc;
}

int iommu_release_device(const pci_dev_t* d) {
    if (!d) return IOMMU_E_ARG;
    uint64_t f = spin_lock_irqsave(&G.lock);
    idom_t* x = dom_find(d->bus, d->dev, d->fn);
    int rc = IOMMU_E_NOENT, disable_master = 0;
    if (x) {
        while (x->nmaps) {
            imap_t *m = &x->maps[x->nmaps - 1u];
            int unmap_rc = IOMMU_OK;
            for (uint32_t p = 0; p < m->pages; ++p)
                if (kira_vtd_domain_unmap_4k(&x->dom, m->iova + ((uint64_t)p << 12), &G.plat) != KIRA_VTD_OK)
                    unmap_rc = IOMMU_E_HW;
            if (unmap_rc != IOMMU_OK) {
                x->blocked = 1;
                disable_master = 1;
                rc = IOMMU_E_HW;
                break; /* retain uncertain mapping records and backing pages */
            }
            x->nmaps--;
        }
        if (!x->nmaps && rc != IOMMU_E_HW) {
            rc = kira_vtd_domain_release(&x->dom, &G.plat) == KIRA_VTD_OK ? IOMMU_OK : IOMMU_E_HW;
            if (rc == IOMMU_OK) zero_bytes(x, sizeof *x);
            else { x->blocked = 1; disable_master = 1; }
        }
    }
    spin_unlock_irqrestore(&G.lock, f);
    if (disable_master) pci_disable_bus_master(d);
    if (rc == IOMMU_OK) { uart_print("[KIRA IOMMU] "); print_bdf(d->bus, d->dev, d->fn); uart_print(" back to passthrough: unrestricted DMA, NOT isolated\n"); }
    return rc;
}

static int range_pages(uint64_t iova, uint64_t len, uint64_t* first, uint32_t* pages) {
    if (!len || iova + len < iova) return -1;
    uint64_t end = iova + len;
    if (end > (1ull << IOMMU_AW_BITS)) return -1;
    *first = iova & ~0xFFFull;
    uint64_t n = (end - *first + 0xFFF) >> 12;
    if (n > 0x100000) return -1;
    *pages = (uint32_t)n;
    return 0;
}

static int overlaps(const idom_t* x, uint64_t first, uint32_t pages) {
    for (uint32_t i = 0; i < x->nmaps; i++) {
        uint64_t a = x->maps[i].iova, b = a + ((uint64_t)x->maps[i].pages << 12);
        uint64_t e = first + ((uint64_t)pages << 12);
        if (first < b && a < e) return 1;
    }
    return 0;
}

static int undo_pages(idom_t* x, uint64_t first, uint32_t done) {
    int rc = IOMMU_OK;
    for (uint32_t i = 0; i < done; i++)
        if (kira_vtd_domain_unmap_4k(&x->dom, first + ((uint64_t)i << 12), &G.plat) != KIRA_VTD_OK)
            rc = IOMMU_E_HW;
    if (rc != IOMMU_OK) x->blocked = 1;
    return rc;
}

int iommu_map(const pci_dev_t* d, uint64_t iova, uint64_t phys, uint64_t len, unsigned rw) {
    uint64_t first; uint32_t pages;
    if (!d) return IOMMU_E_ARG;
    if (!G.active) return IOMMU_E_NONE;
    if ((rw & ~(IOMMU_R | IOMMU_W)) || !rw || ((iova ^ phys) & 0xFFF)) return IOMMU_E_ARG;
    if (range_pages(iova, len, &first, &pages) != 0) return IOMMU_E_RANGE;
    uint64_t pfirst = phys & ~0xFFFull;
    if (phys + len < phys || (G.haw < 64 && phys + len > (1ull << G.haw))) return IOMMU_E_RANGE;
    int rc = IOMMU_OK;
    uint64_t f = spin_lock_irqsave(&G.lock);
    idom_t* x = dom_find(d->bus, d->dev, d->fn);
    if (!x || !x->dom.active || x->blocked) rc = IOMMU_E_NOENT;
    else if (x->nmaps >= IOMMU_MAX_MAPS || overlaps(x, first, pages)) rc = IOMMU_E_EXIST;
    else {
        uint32_t perm = (rw & IOMMU_R ? KIRA_VTD_MAP_READ : 0) | (rw & IOMMU_W ? KIRA_VTD_MAP_WRITE : 0);
        uint32_t i;
        for (i = 0; i < pages; i++) {
            int hr = kira_vtd_domain_map_4k(&x->dom, first + ((uint64_t)i << 12), pfirst + ((uint64_t)i << 12), perm, &G.plat);
            if (hr != KIRA_VTD_OK) { rc = hr == KIRA_VTD_MEMORY_FAILED ? IOMMU_E_NOMEM : IOMMU_E_HW; break; }
        }
        if (rc != IOMMU_OK) {
            uint32_t attempted = i + 1u > pages ? pages : i + 1u;
            if (undo_pages(x, first, attempted) != IOMMU_OK) {
                /* Keep the caller's backing pinned if any PTE/TLB state is uncertain. */
                x->maps[x->nmaps].iova = first;
                x->maps[x->nmaps].pages = attempted;
                x->maps[x->nmaps].rw = rw;
                x->nmaps++;
            }
        } else { x->maps[x->nmaps].iova = first; x->maps[x->nmaps].pages = pages; x->maps[x->nmaps].rw = rw; x->nmaps++; }
    }
    int disable_master = x && x->blocked;
    spin_unlock_irqrestore(&G.lock, f);
    if (disable_master) pci_disable_bus_master(d);
    return rc;
}

int iommu_unmap(const pci_dev_t* d, uint64_t iova, uint64_t len) {
    uint64_t first; uint32_t pages;
    if (!d) return IOMMU_E_ARG;
    if (!G.active) return IOMMU_E_NONE;
    if (range_pages(iova, len, &first, &pages) != 0) return IOMMU_E_RANGE;
    int rc = IOMMU_E_NOENT;
    uint64_t f = spin_lock_irqsave(&G.lock);
    idom_t* x = dom_find(d->bus, d->dev, d->fn);
    for (uint32_t i = 0; x && i < x->nmaps; i++) {
        if (x->maps[i].iova != first || x->maps[i].pages != pages) continue;
        rc = IOMMU_OK;
        for (uint32_t p = 0; p < pages; p++)
            if (kira_vtd_domain_unmap_4k(&x->dom, first + ((uint64_t)p << 12), &G.plat) != KIRA_VTD_OK) rc = IOMMU_E_HW;
        if (rc == IOMMU_OK) x->maps[i] = x->maps[--x->nmaps];
        else x->blocked = 1; /* do not release backing while a stale translation may survive */
        break;
    }
    int disable_master = rc == IOMMU_E_HW;
    spin_unlock_irqrestore(&G.lock, f);
    if (disable_master) pci_disable_bus_master(d);
    return rc;
}

void iommu_unmap_all(const pci_dev_t* d) {
    if (!d || !G.active) return;
    int failed = 0;
    uint64_t f = spin_lock_irqsave(&G.lock);
    idom_t* x = dom_find(d->bus, d->dev, d->fn);
    while (x && x->nmaps) {
        imap_t *m = &x->maps[x->nmaps - 1u];
        int rc = IOMMU_OK;
        for (uint32_t p = 0; p < m->pages; p++)
            if (kira_vtd_domain_unmap_4k(&x->dom, m->iova + ((uint64_t)p << 12), &G.plat) != KIRA_VTD_OK)
                rc = IOMMU_E_HW;
        if (rc != IOMMU_OK) {
            x->blocked = 1;
            failed = 1;
            break; /* retain record and pin backing pages */
        }
        x->nmaps--;
    }
    spin_unlock_irqrestore(&G.lock, f);
    if (failed) pci_disable_bus_master(d);
}

/* ---- Faults ---- */

static void collect_fault(void* ctx, const struct kira_vtd_fault* ft) {
    (void)ctx;
    G.faults_total++;
    idom_t* x = dom_find((uint8_t)(ft->source_id >> 8), (uint8_t)((ft->source_id >> 3) & 31), (uint8_t)(ft->source_id & 7));
    if (x) x->faults++;
    if (G.nbatch < IOMMU_FAULT_BATCH) G.batch[G.nbatch++] = *ft;
    else G.dropped++;
}

int iommu_fault_poll(void) {
    if (!G.active) return 0;
    if (__atomic_exchange_n(&G.polling, 1, __ATOMIC_ACQUIRE)) return 0;      /* another CPU is draining */
    int total = 0;
    struct kira_vtd_fault out[IOMMU_FAULT_BATCH];
    uint32_t n, dropped;
    uint64_t f = spin_lock_irqsave(&G.lock);
    G.nbatch = 0; G.dropped = 0;
    for (int i = 0; i < VTD_MAX_UNITS; i++) {
        if (!G.on[i]) continue;
        int rc = kira_vtd_fault_poll(&G.unit[i], collect_fault, 0);
        if (rc > 0) total += rc;
    }
    n = G.nbatch; dropped = G.dropped;
    for (uint32_t i = 0; i < n; i++) out[i] = G.batch[i];
    spin_unlock_irqrestore(&G.lock, f);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t bus = (uint8_t)(out[i].source_id >> 8), dev = (uint8_t)((out[i].source_id >> 3) & 31), fn = (uint8_t)(out[i].source_id & 7);
        f = spin_lock_irqsave(&G.lock);
        idom_t* x = dom_find(bus, dev, fn);
        int skip = x && x->logged >= IOMMU_FAULT_LOG_PER_DEVICE;
        if (x && !skip) x->logged++;
        spin_unlock_irqrestore(&G.lock, f);
        if (skip) continue;                                          /* counted, not logged */
        uart_print("[KIRA IOMMU] DMA fault: device "); print_bdf(bus, dev, fn);
        uart_print(out[i].is_read ? " read " : " write "); uart_print_hex(out[i].address);
        uart_print(": "); uart_print(kira_vtd_fault_reason(out[i].reason)); uart_print(" (FR "); uart_print_dec(out[i].reason);
        uart_print("), access blocked\n");
    }
    if (dropped) { uart_print("[KIRA IOMMU] "); uart_print_dec(dropped); uart_print(" more fault record(s) not listed\n"); }
    __atomic_store_n(&G.polling, 0, __ATOMIC_RELEASE);
    return total;
}

uint32_t iommu_fault_count(const pci_dev_t* d) {
    if (!d || !G.active) return 0;
    uint64_t f = spin_lock_irqsave(&G.lock);
    idom_t* x = dom_find(d->bus, d->dev, d->fn);
    uint32_t n = x ? x->faults : 0;
    spin_unlock_irqrestore(&G.lock, f);
    return n;
}
