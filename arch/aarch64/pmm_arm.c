/* ARM64 physical page manager. Reserved pages are tracked separately from allocated pages:
   a free operation must never turn firmware/kernel/DMA reservations into allocatable RAM. */
#include "pmm.h"
#include "spinlock.h"
#include "pmm_arm.h"

static uint64_t* bits;
static uint64_t* reserved_bits;
static uint64_t base_pfn, npages, free_pages, total_pages, hint;
static spinlock_t lk = SPINLOCK_INIT;

static int used(uint64_t i) { return (bits[i >> 6] >> (i & 63)) & 1; }
static void set_used(uint64_t i) { bits[i >> 6] |= 1ull << (i & 63); }
static void set_free(uint64_t i) { bits[i >> 6] &= ~(1ull << (i & 63)); }
static int reserved(uint64_t i) { return (reserved_bits[i >> 6] >> (i & 63)) & 1; }
static void set_reserved(uint64_t i) { reserved_bits[i >> 6] |= 1ull << (i & 63); }
static void clear_reserved(uint64_t i) { reserved_bits[i >> 6] &= ~(1ull << (i & 63)); }

uint64_t pmm_arm_map_bytes(uint64_t lo, uint64_t hi) {
    if (hi <= lo) return 0;
    uint64_t pages = (hi - lo) / PMM_PAGE_SIZE;
    if (pages > UINT64_MAX - 63) return 0;
    uint64_t words = (pages + 63) / 64;
    if (words > UINT64_MAX / (2 * sizeof(uint64_t))) return 0;
    return words * 2 * sizeof(uint64_t);
}

void pmm_arm_reserve(uint64_t res_lo, uint64_t res_hi) {
    if (!bits || !reserved_bits || res_hi <= res_lo || !npages) return;
    uint64_t first = res_lo / PMM_PAGE_SIZE;
    uint64_t end = res_hi / PMM_PAGE_SIZE + ((res_hi % PMM_PAGE_SIZE) != 0);
    uint64_t limit = base_pfn + npages;
    if (first < base_pfn) first = base_pfn;
    if (end > limit) end = limit;
    if (first >= end) return;

    uint64_t f = spin_lock_irqsave(&lk);
    for (uint64_t p = first; p < end; p++) {
        uint64_t i = p - base_pfn;
        set_reserved(i);
        if (!used(i)) { set_used(i); free_pages--; }
    }
    spin_unlock_irqrestore(&lk, f);
}

void pmm_arm_init(const pmm_arm_region_t* r, unsigned n, uint64_t lo, uint64_t hi, void* map,
                  uint64_t res_lo, uint64_t res_hi) {
    bits = 0; reserved_bits = 0;
    base_pfn = free_pages = total_pages = hint = 0;
    npages = 0;
    if (!map || hi <= lo || !r || !n) return;

    base_pfn = lo / PMM_PAGE_SIZE;
    npages = (hi - lo) / PMM_PAGE_SIZE;
    if (!npages || npages > UINT64_MAX - 63) { npages = 0; return; }
    uint64_t words = (npages + 63) / 64;
    bits = (uint64_t*)map;
    reserved_bits = bits + words;

    /* Initially every page is unavailable/reserved. Only validated RAM regions clear both maps. */
    for (uint64_t i = 0; i < words; i++) {
        bits[i] = ~0ull;
        reserved_bits[i] = ~0ull;
    }

    uint64_t limit = base_pfn + npages;
    for (unsigned k = 0; k < n; k++) {
        if (!r[k].size || r[k].size > UINT64_MAX - r[k].base) continue;
        uint64_t end = r[k].base + r[k].size;
        uint64_t first = r[k].base / PMM_PAGE_SIZE + ((r[k].base % PMM_PAGE_SIZE) != 0);
        uint64_t last = end / PMM_PAGE_SIZE;
        if (first < base_pfn) first = base_pfn;
        if (last > limit) last = limit;
        for (uint64_t p = first; p < last; p++) {
            uint64_t i = p - base_pfn;
            if (used(i)) {
                set_free(i);
                clear_reserved(i);
                free_pages++;
                total_pages++;
            }
        }
    }
    pmm_arm_reserve(res_lo, res_hi);
}

uint64_t pmm_alloc_page(void) {
    uint64_t f = spin_lock_irqsave(&lk), r = 0;
    for (uint64_t k = 0; k < npages && !r; k++) {
        uint64_t i = (hint + k) % npages;
        if ((i & 63) == 0 && bits[i >> 6] == ~0ull && i + 64 <= npages) { k += 63; continue; }
        if (!used(i) && !reserved(i)) { set_used(i); free_pages--; hint = i + 1; r = (base_pfn + i) * PMM_PAGE_SIZE; }
    }
    spin_unlock_irqrestore(&lk, f);
    return r;
}

int pmm_free_page(uint64_t pa) {
    if ((pa & (PMM_PAGE_SIZE - 1)) != 0) return -1;
    uint64_t p = pa / PMM_PAGE_SIZE;
    if (p < base_pfn || p - base_pfn >= npages) return -1;
    uint64_t f = spin_lock_irqsave(&lk);
    uint64_t i = p - base_pfn;
    int ok = used(i) && !reserved(i);
    if (ok) { set_free(i); free_pages++; if (i < hint) hint = i; }
    spin_unlock_irqrestore(&lk, f);
    return ok ? 0 : -1;
}

uint64_t pmm_alloc_contiguous(uint32_t pages) {
    if (!pages || !npages) return 0;
    uint64_t f = spin_lock_irqsave(&lk), r = 0, run = 0;
    for (uint64_t i = 0; i < npages; i++) {
        if (used(i) || reserved(i)) { run = 0; continue; }
        if (++run == pages) {
            uint64_t s = i + 1 - pages;
            for (uint64_t j = s; j <= i; j++) set_used(j);
            free_pages -= pages;
            r = (base_pfn + s) * PMM_PAGE_SIZE;
            hint = i + 1;
            break;
        }
    }
    spin_unlock_irqrestore(&lk, f);
    return r;
}

int pmm_free_contiguous(uint64_t pa, uint32_t pages) {
    if (!pages || (pa & (PMM_PAGE_SIZE - 1))) return -1;
    uint64_t start = pa / PMM_PAGE_SIZE;
    if (start < base_pfn || start - base_pfn >= npages) return -1;
    uint64_t offset = start - base_pfn;
    if ((uint64_t)pages > npages - offset) return -1;

    uint64_t f = spin_lock_irqsave(&lk);
    int ok = 1;
    for (uint64_t i = offset; i < offset + pages; i++) {
        if (!used(i) || reserved(i)) { ok = 0; break; }
    }
    if (ok) {
        for (uint64_t i = offset; i < offset + pages; i++) set_free(i);
        free_pages += pages;
        if (offset < hint) hint = offset;
    }
    spin_unlock_irqrestore(&lk, f);
    return ok ? 0 : -1;
}

uint32_t pmm_get_free_memory_kb(void) { return (uint32_t)(free_pages * 4); }
uint32_t pmm_get_total_memory_kb(void) { return (uint32_t)(total_pages * 4); }
