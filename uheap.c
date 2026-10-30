#include "uheap.h"

uint64_t uheap_quota_pages(uint64_t total_kb) {
    uint64_t q = total_kb / 2 / 4;                          /* половина ОЗУ в страницах */
    uint64_t window = (UHEAP_VA_END - UHEAP_VA) / 4096ULL;
    if (q < UHEAP_MIN_QUOTA) q = UHEAP_MIN_QUOTA;
    if (q > window) q = window;
    return q;
}

uint64_t uheap_reserve_kb(uint64_t total_kb) {
    uint64_t r = total_kb / 16;
    return r < UHEAP_MIN_RESERVE_KB ? UHEAP_MIN_RESERVE_KB : r;
}

int uheap_check(uint64_t cur_pages, uint64_t req_pages, uint64_t free_kb, uint64_t total_kb) {
    uint64_t quota = uheap_quota_pages(total_kb);
    if (req_pages == 0 || req_pages > quota) return UHEAP_E_ARG;
    if (cur_pages > quota || cur_pages + req_pages > quota) return UHEAP_E_QUOTA;
    uint64_t tables = req_pages / 512 + 3;                  /* PT на каждые 512 страниц + PD/PDPT, если границы пересечены */
    uint64_t need_kb = (req_pages + tables) * 4 + uheap_reserve_kb(total_kb);
    if (free_kb < need_kb) return UHEAP_E_MEM;
    return UHEAP_OK;
}
