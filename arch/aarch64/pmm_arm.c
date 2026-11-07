/* Менеджер физических страниц ARM64: битовая карта над всеми участками ОЗУ из дерева устройств (бит = страница
   занята). Занятым помечается всё, кроме ОЗУ, и в ОЗУ - ядро, дерево устройств и ранние выделения (таблицы MMU,
   стеки ядер, сама карта). Тот же интерфейс pmm.h, что и на x86-64: куча ядра работает поверх него без изменений. */
#include "pmm.h"
#include "spinlock.h"
#include "pmm_arm.h"

static uint64_t* bits;
static uint64_t* reserved;
static uint64_t base_pfn, npages, free_pages, total_pages, hint;
static spinlock_t lk = SPINLOCK_INIT;

static int used(uint64_t i) { return (bits[i >> 6] >> (i & 63)) & 1; }
static void set_used(uint64_t i) { bits[i >> 6] |= 1ull << (i & 63); }
static void set_free(uint64_t i) { bits[i >> 6] &= ~(1ull << (i & 63)); }
static int is_reserved(uint64_t i) { return (reserved[i >> 6] >> (i & 63)) & 1; }
static void set_reserved(uint64_t i) { reserved[i >> 6] |= 1ull << (i & 63); }
static void clear_reserved(uint64_t i) { reserved[i >> 6] &= ~(1ull << (i & 63)); }

uint64_t pmm_arm_map_bytes(uint64_t lo, uint64_t hi) {
    if (hi <= lo) return 0;
    uint64_t pages = (hi - lo) / 4096;
    if (pages > UINT64_MAX - 63) return 0;
    uint64_t words = (pages + 63) / 64;
    return words > UINT64_MAX / 16 ? 0 : words * 16;
}

void pmm_arm_reserve(uint64_t res_lo, uint64_t res_hi) {
    if (res_hi <= res_lo || res_hi > UINT64_MAX - 4095) return;
    uint64_t first = res_lo / 4096, end = (res_hi + 4095) / 4096;
    for (uint64_t p = first; p < end; p++) {
        if (p < base_pfn || p >= base_pfn + npages) continue;
        uint64_t i = p - base_pfn;
        if (!used(i)) { set_used(i); free_pages--; }
        set_reserved(i);
    }
}

void pmm_arm_init(const pmm_arm_region_t* r, unsigned n, uint64_t lo, uint64_t hi, void* map,
                  uint64_t res_lo, uint64_t res_hi) {
    if (!map || !r || !n || hi <= lo || lo > UINT64_MAX - 4095) { bits = reserved = 0; return; }
    base_pfn = lo / 4096; npages = (hi - lo) / 4096;
    bits = (uint64_t*)map;
    uint64_t words = (npages + 63) / 64;
    reserved = bits + words;
    for (uint64_t i = 0; i < words; i++) { bits[i] = ~0ull; reserved[i] = ~0ull; }
    free_pages = total_pages = 0; hint = 0;
    for (unsigned k = 0; k < n; k++) {
        if (!r[k].size || r[k].base > UINT64_MAX - r[k].size || r[k].base > UINT64_MAX - 4095) continue;
        uint64_t a = (r[k].base + 4095) / 4096, b = (r[k].base + r[k].size) / 4096;
        if (a < base_pfn) a = base_pfn;
        if (b > base_pfn + npages) b = base_pfn + npages;
        for (uint64_t p = a; p < b; p++) {
            uint64_t i = p - base_pfn;
            set_free(i); clear_reserved(i); free_pages++; total_pages++;
        }
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
    uint64_t i = p - base_pfn;
    int ok = used(i) && !is_reserved(i);
    if (ok) { set_free(i); free_pages++; }
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
    if (!pages || (pa & 4095) || pa > UINT64_MAX - (uint64_t)pages * 4096) return -1;
    uint64_t first = pa / 4096;
    if (first < base_pfn || first + pages > base_pfn + npages) return -1;
    uint64_t f = spin_lock_irqsave(&lk);
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t p = first + i - base_pfn;
        if (!used(p) || is_reserved(p)) { spin_unlock_irqrestore(&lk, f); return -1; }
    }
    for (uint32_t i = 0; i < pages; i++) set_free(first + i - base_pfn);
    free_pages += pages;
    spin_unlock_irqrestore(&lk, f);
    return 0;
}

uint32_t pmm_get_free_memory_kb(void) { return (uint32_t)(free_pages * 4); }
uint32_t pmm_get_total_memory_kb(void) { return (uint32_t)(total_pages * 4); }
