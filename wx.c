#include "wx.h"

#define PAGE   4096ULL
#define HUGE2M (2ULL << 20)
#define GB     (1ULL << 30)
#define ADDR   0x000FFFFFFFFFF000ULL
#define ADDR2M 0x000FFFFFFFE00000ULL
#define PAT2M  (1ULL << 12)                    /* бит PAT в 2-МБ записи (в 4-КБ записи PAT - бит 7) */
#define LOW7   0x7FULL                         /* P, W, U, PWT, PCD, A, D */

enum { CLS_DATA, CLS_TEXT, CLS_RO, CLS_EXEC };

static int cls_of(const wx_layout_t* l, uint64_t va) {
    if (va >= l->exec_lo && va < l->exec_hi) return CLS_EXEC;
    if (va >= l->text_lo && va < l->text_hi) return CLS_TEXT;
    if (va >= l->ro_lo && va < l->ro_hi) return CLS_RO;
    return CLS_DATA;
}

static uint64_t perm_of(int cls) {
    if (cls == CLS_TEXT || cls == CLS_EXEC) return WX_P;
    if (cls == CLS_RO) return WX_P | WX_NX;
    return WX_P | WX_W | WX_NX;
}

static int hits(uint64_t lo, uint64_t hi, uint64_t a, uint64_t b) { return lo < hi && a < hi && lo < b; }

// Диапазон [a, b) пересекает образ ядра или страницу кода вне образа - там права нужны постранично.
static int special(const wx_layout_t* l, uint64_t a, uint64_t b) {
    return hits(l->text_lo, l->text_hi, a, b) || hits(l->ro_lo, l->ro_hi, a, b) ||
           hits(l->data_lo, l->data_hi, a, b) || hits(l->exec_lo, l->exec_hi, a, b);
}

int wx_enforce(const wx_env_t* env, uint64_t* pml4, const wx_layout_t* l) {
    if (!(pml4[0] & WX_P)) return -1;
    uint64_t* pdpt = env->ptr(pml4[0] & ADDR);
    for (unsigned gi = 0; gi < 512; gi++) {
        uint64_t ge = pdpt[gi];
        if (!(ge & WX_P)) continue;
        uint64_t gbase = (uint64_t)gi * GB;
        if (ge & WX_PS) {
            if (special(l, gbase, gbase + GB)) return -1;
            pdpt[gi] = ge | WX_NX;
            continue;
        }
        uint64_t* pd = env->ptr(ge & ADDR);
        for (unsigned di = 0; di < 512; di++) {
            uint64_t pe = pd[di];
            if (!(pe & WX_P)) continue;
            uint64_t va = gbase + (uint64_t)di * HUGE2M;
            if (pe & WX_PS) {
                if (!special(l, va, va + HUGE2M)) { pd[di] = pe | WX_NX; continue; }
                if (pe & PAT2M) return -1;
                uint64_t pt_phys = env->alloc();
                if (!pt_phys) return -1;
                uint64_t* pt = env->ptr(pt_phys);
                for (unsigned ti = 0; ti < 512; ti++) {
                    uint64_t pva = va + (uint64_t)ti * PAGE;
                    pt[ti] = ((pe & ADDR2M) + (uint64_t)ti * PAGE) | (pe & LOW7 & ~WX_W) | perm_of(cls_of(l, pva));
                }
                pd[di] = pt_phys | WX_P | WX_W;       // таблица целиком готова до подмены записи: отображение не пропадает
            } else {
                uint64_t* pt = env->ptr(pe & ADDR);
                for (unsigned ti = 0; ti < 512; ti++) {
                    uint64_t pte = pt[ti];
                    if (!(pte & WX_P)) continue;
                    uint64_t want = perm_of(cls_of(l, va + (uint64_t)ti * PAGE));
                    pt[ti] = (pte & ~(WX_W | WX_NX)) | (want & (WX_W | WX_NX));
                }
            }
        }
    }
    return 0;
}

static void leaf(wx_stats_t* st, uint64_t va, uint64_t size, int w, int nx) {
    if (w && !nx) {
        if (!st->bad_count) st->bad_va = va;
        st->bad_count++;
    } else if (!nx) st->exec_kb += size >> 10;
    else if (w) st->rw_kb += size >> 10;
    else st->ro_kb += size >> 10;
}

static void walk(const wx_env_t* env, const uint64_t* t, int level, uint64_t base, int w, int nx, wx_stats_t* st) {
    unsigned shift = 12 + 9 * (unsigned)(level - 1);
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = t[i];
        if (!(e & WX_P)) continue;
        uint64_t va = base | ((uint64_t)i << shift);
        if (level == 4 && i >= 256) va |= 0xFFFF000000000000ULL;
        int ew = w && (e & WX_W), enx = nx || (e & WX_NX) != 0;
        if (level == 1 || (level < 4 && (e & WX_PS))) leaf(st, va, 1ULL << shift, ew, enx);
        else walk(env, env->ptr(e & ADDR), level - 1, va, ew, enx, st);
    }
}

// Эффективные права адреса va: 0 - есть отображение, -1 - нет.
static int lookup(const wx_env_t* env, const uint64_t* pml4, uint64_t va, int* w, int* nx) {
    const uint64_t* t = pml4;
    int ew = 1, enx = 0;
    for (int level = 4; level >= 1; level--) {
        uint64_t e = t[(va >> (12 + 9 * (level - 1))) & 0x1FF];
        if (!(e & WX_P)) return -1;
        ew = ew && (e & WX_W) != 0;
        enx = enx || (e & WX_NX) != 0;
        if (level == 1 || (level < 4 && (e & WX_PS))) { *w = ew; *nx = enx; return 0; }
        t = env->ptr(e & ADDR);
    }
    return -1;
}

static void check_range(const wx_env_t* env, const uint64_t* pml4, wx_stats_t* st, uint64_t lo, uint64_t hi, int want_w, int want_nx) {
    for (uint64_t va = lo; va < hi; va += PAGE) {
        int w, nx;
        if (lookup(env, pml4, va, &w, &nx) != 0 || w != want_w || nx != want_nx) {
            if (st->layout_ok) st->layout_bad_va = va;
            st->layout_ok = 0;
            return;
        }
    }
}

int wx_scan(const wx_env_t* env, const uint64_t* pml4, const wx_layout_t* l, wx_stats_t* st) {
    st->exec_kb = st->ro_kb = st->rw_kb = 0;
    st->bad_count = st->bad_va = 0;
    st->layout_ok = 1;
    st->layout_bad_va = 0;
    walk(env, pml4, 4, 0, 1, 0, st);
    check_range(env, pml4, st, l->text_lo, l->text_hi, 0, 0);
    check_range(env, pml4, st, l->exec_lo, l->exec_hi, 0, 0);
    check_range(env, pml4, st, l->ro_lo, l->ro_hi, 0, 1);
    check_range(env, pml4, st, l->data_lo, l->data_hi, 1, 1);
    return (st->bad_count == 0 && st->layout_ok) ? 0 : -1;
}
