#include "ramext.h"

#define E_PRESENT (1ULL << 0)
#define E_WRITE   (1ULL << 1)
#define E_HUGE    (1ULL << 7)
#define E_NX      (1ULL << 63)
#define E_ADDR    0x000FFFFFFFFFF000ULL
#define HUGE_SIZE (2ULL << 20)
#define GB        (1ULL << 30)

uint64_t ramext_map(uint64_t* pdpt, uint64_t start, uint64_t end,
                    uint64_t (*alloc)(void), uint64_t* mapped_start) {
    uint64_t s = (start + HUGE_SIZE - 1) & ~(HUGE_SIZE - 1);
    uint64_t e = end & ~(HUGE_SIZE - 1);
    *mapped_start = s;
    if (end < start || e <= s || e > (512 * GB) || s < GB) { *mapped_start = 0; return 0; }
    uint64_t a = s;
    while (a < e) {
        unsigned gi = (unsigned)(a / GB);
        uint64_t ge = (uint64_t)(gi + 1) * GB;
        uint64_t* pd;
        uint64_t pe = pdpt[gi];
        if (!(pe & E_PRESENT)) {
            uint64_t pg = alloc();
            if (!pg) break;
            pd = (uint64_t*)(uintptr_t)pg;
            for (unsigned i = 0; i < 512; i++) pd[i] = 0;
            pdpt[gi] = pg | E_PRESENT | E_WRITE;
        } else if (pe & E_HUGE) {
            break;                                            // гигабайт уже отображён целиком кем-то другим
        } else {
            pd = (uint64_t*)(uintptr_t)(pe & E_ADDR);
        }
        uint64_t lim = e < ge ? e : ge;
        int stop = 0;
        for (; a < lim; a += HUGE_SIZE) {
            unsigned di = (unsigned)((a % GB) / HUGE_SIZE);
            if (pd[di] & E_PRESENT) { stop = 1; break; }
            pd[di] = a | E_PRESENT | E_WRITE | E_HUGE | E_NX;
        }
        if (stop) break;
    }
    if (a == s) { *mapped_start = s; return s; }
    return a;
}

/* Как ramext_map, но [start, end) расширяется НАРУЖУ до 2 МБ, права только чтение + NX, занятые записи пропускаются
   (там уже отображено). 1 - всё отображено, 0 - нет памяти под PD или адрес вне identity-карты (>= 512 ГБ). */
int ramext_map_ro(uint64_t* pdpt, uint64_t start, uint64_t end, uint64_t (*alloc)(void)) {
    uint64_t s = start & ~(HUGE_SIZE - 1), e = (end + HUGE_SIZE - 1) & ~(HUGE_SIZE - 1);
    if (end <= start || e > 512 * GB || e < end) return 0;
    for (uint64_t a = s; a < e; ) {
        unsigned gi = (unsigned)(a / GB);
        uint64_t* pd;
        uint64_t pe = pdpt[gi];
        if (!(pe & E_PRESENT)) {
            uint64_t pg = alloc();
            if (!pg) return 0;
            pd = (uint64_t*)(uintptr_t)pg;
            for (unsigned i = 0; i < 512; i++) pd[i] = 0;
            pdpt[gi] = pg | E_PRESENT | E_WRITE;
        } else if (pe & E_HUGE) {
            a = (uint64_t)(gi + 1) * GB; continue;
        } else pd = (uint64_t*)(uintptr_t)(pe & E_ADDR);
        uint64_t lim = e < (uint64_t)(gi + 1) * GB ? e : (uint64_t)(gi + 1) * GB;
        for (; a < lim; a += HUGE_SIZE) {
            unsigned di = (unsigned)((a % GB) / HUGE_SIZE);
            if (!(pd[di] & E_PRESENT)) pd[di] = a | E_PRESENT | E_HUGE | E_NX;
        }
    }
    return 1;
}

#ifndef KIRA_HOST_TEST
#include "pmm.h"
#include "vmm.h"
#include "uart.h"

int ramext_map_read(uint64_t phys, uint64_t len) {
    if (len == 0 || phys + len < phys) return 0;
    static pmm_range_t r[64];                       /* только при старте (таблицы ACPI), один поток */
    unsigned n = pmm_acpi_ranges(r, 64);
    for (unsigned i = 0; i < n; i++) {
        if (phys >= r[i].start && phys + len <= r[i].end) {
            uint64_t* pml4 = (uint64_t*)(uintptr_t)(vmm_kernel_root() & E_ADDR);
            if (!(pml4[0] & E_PRESENT)) return 0;
            return ramext_map_ro((uint64_t*)(uintptr_t)(pml4[0] & E_ADDR), phys, phys + len, pmm_alloc_page);
        }
    }
    return 0;
}

uint64_t ramext_init(void) {
    static pmm_range_t r[PMM_MAX_HIGH];             /* 4 КБ - не на стек; вызывается один раз при старте */
    unsigned n = pmm_high_ranges(r, PMM_MAX_HIGH);
    uint64_t* pml4 = (uint64_t*)(uintptr_t)(vmm_kernel_root() & E_ADDR);
    if (!(pml4[0] & E_PRESENT)) {
        uart_print("[KIRA RAM] Identity map root missing: memory above 1 GB stays unused.\n");
        return 0;
    }
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4[0] & E_ADDR);
    uint64_t added_kb = 0, lost_kb = 0;
    for (unsigned i = 0; i < n; i++) {
        uint64_t ms, me = ramext_map(pdpt, r[i].start, r[i].end, pmm_alloc_page, &ms);
        int64_t pages = (me > ms) ? pmm_add_high(ms, me) : 0;
        if (pages < 0) pages = 0;
        added_kb += (uint64_t)pages * 4;
        lost_kb += ((r[i].end - r[i].start) >> 10) - (uint64_t)pages * 4;
    }
    uart_print("[KIRA RAM] Mapped RAM above 1 GB: ");
    uart_print_dec(added_kb >> 10);
    uart_print(" MB (2 MB pages, no-execute); edges lost to alignment or failures: ");
    uart_print_dec(lost_kb);
    uart_print(" KB.\n");
    return added_kb >> 10;
}
#endif
