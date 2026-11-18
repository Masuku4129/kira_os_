#include "earlymap.h"

#define PAGE   4096ULL
#define HUGE2M (2ULL << 20)
#define ADDR   0x000FFFFFFFFFF000ULL

static int hits(uint64_t lo, uint64_t hi, uint64_t a, uint64_t b) { return lo < hi && a < hi && lo < b; }
static int inside(uint64_t lo, uint64_t hi, uint64_t v) { return lo < hi && v >= lo && v < hi; }

// Права страницы по её классу: исполняемое (текст, трамплин, мост) - RX; константы - R+NX; всё прочее - RW+NX.
static uint64_t perm_of(const earlymap_layout_t* l, uint64_t va) {
    if (inside(l->w.text_lo, l->w.text_hi, va) || inside(l->w.exec_lo, l->w.exec_hi, va) ||
        inside(l->exec2_lo, l->exec2_hi, va)) return WX_P;
    if (inside(l->w.ro_lo, l->w.ro_hi, va)) return WX_P | WX_NX;
    return WX_P | WX_W | WX_NX;
}

// Области, где права разные внутри 2 МБ: образ, трамплин, мост.
static int special(const earlymap_layout_t* l, uint64_t a, uint64_t b) {
    return hits(l->image_lo, l->image_hi, a, b) || hits(l->w.exec_lo, l->w.exec_hi, a, b) ||
           hits(l->exec2_lo, l->exec2_hi, a, b);
}

int earlymap_build(const wx_env_t* env, const earlymap_pa_t* pa, const earlymap_layout_t* l) {
    if ((l->w.text_lo | l->w.text_hi | l->w.ro_lo | l->w.ro_hi | l->w.data_lo | l->w.data_hi |
         l->image_lo | l->image_hi | l->w.exec_lo | l->w.exec_hi | l->exec2_lo | l->exec2_hi) & (PAGE - 1)) return -1;
    if (l->image_lo >= l->image_hi || l->image_hi > EARLYMAP_LIMIT) return -1;
    uint64_t* pml4 = env->ptr(pa->pml4);
    uint64_t* pdpt = env->ptr(pa->pdpt);
    uint64_t* pd = env->ptr(pa->pd);
    for (unsigned i = 0; i < 512; i++) pml4[i] = pdpt[i] = 0;
    pml4[0] = pa->pdpt | WX_P | WX_W;
    pdpt[0] = pa->pd | WX_P | WX_W;
    unsigned used = 0;
    for (unsigned r = 0; r < 512; r++) {
        uint64_t va = (uint64_t)r * HUGE2M;
        if (!special(l, va, va + HUGE2M)) {
            pd[r] = va | WX_P | WX_W | WX_PS | WX_NX;               // обычная память: 2-МБ страница RW+NX
            continue;
        }
        if (used >= pa->pt_pool_n) return -2;
        uint64_t pt_pa = pa->pt_pool + (uint64_t)used * PAGE;
        used++;
        uint64_t* pt = env->ptr(pt_pa);
        for (unsigned t = 0; t < 512; t++) {
            uint64_t pva = va + (uint64_t)t * PAGE;
            pt[t] = pva | perm_of(l, pva);
        }
        pd[r] = pt_pa | WX_P | WX_W;
    }
    return 0;
}

int earlymap_make_data(const wx_env_t* env, uint64_t pml4_pa, uint64_t va) {
    if (va >= EARLYMAP_LIMIT || (va & (PAGE - 1))) return -1;
    const uint64_t* pml4 = env->ptr(pml4_pa);
    if (!(pml4[0] & WX_P)) return -1;
    const uint64_t* pdpt = env->ptr(pml4[0] & ADDR);
    if (!(pdpt[0] & WX_P) || (pdpt[0] & WX_PS)) return -1;
    const uint64_t* pd = env->ptr(pdpt[0] & ADDR);
    uint64_t e = pd[(va >> 21) & 0x1FF];
    if (!(e & WX_P) || (e & WX_PS)) return -1;
    uint64_t* pt = env->ptr(e & ADDR);
    unsigned ti = (unsigned)((va >> 12) & 0x1FF);
    if (!(pt[ti] & WX_P)) return -1;
    pt[ti] |= WX_W | WX_NX;
    return 0;
}
