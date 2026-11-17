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

/* Строка «пределы машины» в описании железа (hw_set_describe_extra): куча программы и резерв ядра. */
void uheap_describe_limits(const hw_profile_t* p, void (*out)(const char*)) {
    char b[120]; unsigned n = 0;
    #define PUTS(s) do { const char* q_ = (s); while (*q_ && n < sizeof b - 2) b[n++] = *q_++; } while (0)
    #define PUTU(x) do { uint64_t v_ = (x); char t_[21]; int k_ = 0; do { t_[k_++] = (char)('0' + v_ % 10); v_ /= 10; } while (v_); \
                         while (k_ && n < sizeof b - 2) b[n++] = t_[--k_]; } while (0)
    PUTS("[KIRA HW] limits for this machine: program heap up to "); PUTU(uheap_quota_pages(p->ram_kb) * 4 / 1024);
    PUTS(" MB per program, kernel keeps "); PUTU(uheap_reserve_kb(p->ram_kb) / 1024); PUTS(" MB free");
    #undef PUTS
    #undef PUTU
    b[n++] = '\n'; b[n] = 0;
    out(b);
}
