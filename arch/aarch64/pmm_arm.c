/* Менеджер физических страниц ARM64: битовая карта над всеми участками ОЗУ из дерева устройств (бит = страница
   занята). Занятым помечается всё, кроме ОЗУ, и в ОЗУ - ядро, дерево устройств и ранние выделения (таблицы MMU,
   стеки ядер, сама карта). Тот же интерфейс pmm.h, что и на x86-64: куча ядра работает поверх него без изменений. */
#include "pmm.h"
#include "spinlock.h"
#include "pmm_arm.h"

static uint64_t* bits;
static uint64_t base_pfn, npages, free_pages, total_pages, hint;
static spinlock_t lk = SPINLOCK_INIT;

static int used(uint64_t i) { return (bits[i >> 6] >> (i & 63)) & 1; }
static void set_used(uint64_t i) { bits[i >> 6] |= 1ull << (i & 63); }
static void set_free(uint64_t i) { bits[i >> 6] &= ~(1ull << (i & 63)); }

uint64_t pmm_arm_map_bytes(uint64_t lo, uint64_t hi) { return (((hi - lo) / 4096 + 63) / 64) * 8; }

void pmm_arm_reserve(uint64_t res_lo, uint64_t res_hi) {
    for (uint64_t p = res_lo / 4096; p < (res_hi + 4095) / 4096; p++)
        if (p >= base_pfn && p < base_pfn + npages && !used(p - base_pfn)) { set_used(p - base_pfn); free_pages--; }
}

void pmm_arm_init(const pmm_arm_region_t* r, unsigned n, uint64_t lo, uint64_t hi, void* map,
                  uint64_t res_lo, uint64_t res_hi) {
    base_pfn = lo / 4096; npages = (hi - lo) / 4096;
    bits = (uint64_t*)map;
    for (uint64_t i = 0; i < (npages + 63) / 64; i++) bits[i] = ~0ull;
    free_pages = total_pages = 0;
    for (unsigned k = 0; k < n; k++) {
        uint64_t a = (r[k].base + 4095) / 4096, b = (r[k].base + r[k].size) / 4096;
        for (uint64_t p = a; p < b; p++) { set_free(p - base_pfn); free_pages++; total_pages++; }
    }
    pmm_arm_reserve(res_lo, res_hi);
}

uint64_t pmm_alloc_page(void) {
    uint64_t f = spin_lock_irqsave(&lk), r = 0;
    for (uint64_t k = 0; k < npages && !r; k++) {
        uint64_t i = (hint + k) % npages;
        if ((i & 63) == 0 && bits[i >> 6] == ~0ull && i + 64 <= npages) { k += 63; continue; }
        if (!used(i)) { set_used(i); free_pages--; hint = i + 1; r = (base_pfn + i) * 4096; }
    }
    spin_unlock_irqrestore(&lk, f);
    return r;
}

int pmm_free_page(uint64_t pa) {
    uint64_t p = pa / 4096;
    if ((pa & 4095) || p < base_pfn || p >= base_pfn + npages) return -1;
    uint64_t f = spin_lock_irqsave(&lk);
    int ok = used(p - base_pfn);
    if (ok) { set_free(p - base_pfn); free_pages++; }
    spin_unlock_irqrestore(&lk, f);
    return ok ? 0 : -1;
}

uint64_t pmm_alloc_contiguous(uint32_t pages) {
    if (!pages) return 0;
    uint64_t f = spin_lock_irqsave(&lk), r = 0, run = 0;
    for (uint64_t i = 0; i < npages; i++) {
        if (used(i)) { run = 0; continue; }
        if (++run == pages) {
            uint64_t s = i + 1 - pages;
            for (uint64_t j = s; j <= i; j++) set_used(j);
            free_pages -= pages;
            r = (base_pfn + s) * 4096;
            break;
        }
    }
    spin_unlock_irqrestore(&lk, f);
    return r;
}

int pmm_free_contiguous(uint64_t pa, uint32_t pages) {
    for (uint32_t i = 0; i < pages; i++) if (pmm_free_page(pa + (uint64_t)i * 4096)) return -1;
    return 0;
}

uint32_t pmm_get_free_memory_kb(void) { return (uint32_t)(free_pages * 4); }
uint32_t pmm_get_total_memory_kb(void) { return (uint32_t)(total_pages * 4); }
