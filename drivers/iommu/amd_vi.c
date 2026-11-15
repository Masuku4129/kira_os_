/* AMD-Vi: чистая логика (см. amd_vi.h). Формат по спецификации «AMD I/O Virtualization Technology (IOMMU) Specification». */
#include "amd_vi.h"

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

enum { IVHD_10 = 0x10, IVHD_11 = 0x11, IVHD_40 = 0x40, IVMD_ALL = 0x20, IVMD_ONE = 0x21, IVMD_RANGE = 0x22 };

static int add_range(amdvi_info_t* o, unsigned unit, uint16_t lo, uint16_t hi, int alias) {
    if (lo > hi) return AMDVI_E_FORMAT;
    if (o->nranges >= AMDVI_MAX_RANGES) return AMDVI_E_LIMIT;
    o->range[o->nranges++] = (amdvi_range_t){ lo, hi, (uint8_t)unit, (uint8_t)alias };
    return 0;
}

/* Записи блока IVHD: 0x00-0x3F - 4 байта, 0x40-0x7F - 8 байт, 0x80 и выше - переменной длины (длина UID в байте 21). */
static int parse_ivhd_entries(amdvi_info_t* o, unsigned unit, const uint8_t* e, size_t n) {
    size_t i = 0;
    int have_start = 0; uint16_t start = 0; int start_alias = 0;
    while (i < n) {
        uint8_t t = e[i];
        size_t sz = t < 0x40 ? 4 : t < 0x80 ? 8 : 0;
        if (t >= 0x80) { if (n - i < 22) return AMDVI_E_SHORT; sz = 22 + (size_t)e[i + 21]; }
        if (sz > n - i) return AMDVI_E_SHORT;
        uint16_t dev = rd16(e + i + 1);
        int rc = 0;
        switch (t) {
        case 1:  rc = add_range(o, unit, 0, 0xFFFF, 0); break;                       /* все устройства */
        case 2:  rc = add_range(o, unit, dev, dev, 0); break;                        /* одно */
        case 3:  have_start = 1; start = dev; start_alias = 0; break;                /* начало диапазона */
        case 4:  if (!have_start) return AMDVI_E_FORMAT; rc = add_range(o, unit, start, dev, start_alias); have_start = 0; break;
        case 0x42: rc = add_range(o, unit, dev, dev, 1); break;                      /* устройство с псевдонимом запроса */
        case 0x43: have_start = 1; start = dev; start_alias = 1; break;
        case 0x48:                                                                    /* особое устройство: IOAPIC, HPET */
            if (o->nspecial >= AMDVI_MAX_SPECIAL) return AMDVI_E_LIMIT;
            o->special[o->nspecial++] = (amdvi_special_t){ rd16(e + i + 4), e[i + 6], e[i + 3] };
            break;
        default: break;                                                               /* неизвестное - по длине */
        }
        if (rc) return rc;
        i += sz;
    }
    return have_start ? AMDVI_E_FORMAT : 0;
}

int amdvi_ivrs_parse(const void* table, size_t len, amdvi_info_t* o) {
    const uint8_t* t = (const uint8_t*)table;
    if (!t || !o) return AMDVI_E_ARG;
    for (size_t k = 0; k < sizeof *o; k++) ((uint8_t*)o)[k] = 0;
    if (len < 48) return AMDVI_E_SHORT;
    if (!(t[0] == 'I' && t[1] == 'V' && t[2] == 'R' && t[3] == 'S')) return AMDVI_E_FORMAT;
    uint32_t tl = rd32(t + 4);
    if (tl < 48 || tl > len) return AMDVI_E_SHORT;
    uint8_t sum = 0;
    for (uint32_t k = 0; k < tl; k++) sum = (uint8_t)(sum + t[k]);
    if (sum) return AMDVI_E_FORMAT;
    o->ivinfo = rd32(t + 36);
    size_t p = 48;
    while (p + 4 <= tl) {
        uint8_t type = t[p];
        size_t bl = rd16(t + p + 2);
        if (bl < 4 || bl > tl - p) return AMDVI_E_SHORT;
        const uint8_t* b = t + p;
        if (type == IVHD_10 || type == IVHD_11 || type == IVHD_40) {
            size_t hs = type == IVHD_10 ? 24 : 40;
            if (bl < hs) return AMDVI_E_SHORT;
            uint16_t seg = rd16(b + 16);
            if (seg == 0) {                                   /* другие сегменты PCI не поддержаны: блок пропускается, устройства остаются без IOMMU */
                if (o->nunits >= AMDVI_MAX_UNITS) return AMDVI_E_LIMIT;
                unsigned u = o->nunits++;
                o->unit[u].devid = rd16(b + 4);
                o->unit[u].base = rd64(b + 8);
                o->unit[u].type = type;
                o->unit[u].efr = type == IVHD_10 ? 0 : rd64(b + 24);
                if (!o->unit[u].base || (o->unit[u].base & 0x3FFF)) return AMDVI_E_FORMAT;   /* регистры - 16 КБ, выровнены */
                int rc = parse_ivhd_entries(o, u, b + hs, bl - hs);
                if (rc) return rc;
            }
        } else if (type == IVMD_ALL || type == IVMD_ONE || type == IVMD_RANGE) {
            if (bl < 32) return AMDVI_E_SHORT;
            if (o->nivmd >= AMDVI_MAX_IVMD) return AMDVI_E_LIMIT;
            uint16_t d = rd16(b + 4), aux = rd16(b + 6);
            amdvi_ivmd_t m = { 0, 0xFFFF, rd64(b + 16), rd64(b + 24), b[1] };
            if (type == IVMD_ONE) m.lo = m.hi = d;
            else if (type == IVMD_RANGE) { m.lo = d; m.hi = aux; if (m.lo > m.hi) return AMDVI_E_FORMAT; }
            o->ivmd[o->nivmd++] = m;
        }
        p += bl;
    }
    return o->nunits ? 0 : AMDVI_E_NOUNIT;
}

int amdvi_unit_of(const amdvi_info_t* i, uint16_t devid) {
    int found = -1;
    for (unsigned k = 0; k < i->nranges; k++) {
        const amdvi_range_t* r = &i->range[k];
        if (devid < r->lo || devid > r->hi) continue;
        if (r->alias) found = -2;                      /* запросы идут под другим номером: DTE пришлось бы вести для псевдонима */
        else return r->unit;
    }
    return found;
}
int amdvi_ivmd_hit(const amdvi_info_t* i, uint16_t devid) {
    for (unsigned k = 0; k < i->nivmd; k++) if (devid >= i->ivmd[k].lo && devid <= i->ivmd[k].hi) return 1;
    return 0;
}
int amdvi_is_special(const amdvi_info_t* i, uint16_t devid) {
    for (unsigned k = 0; k < i->nspecial; k++) if (i->special[k].devid == devid) return 1;
    return 0;
}

/* ---- DTE ---- */
#define DTE_V   (1ull << 0)
#define DTE_TV  (1ull << 1)
#define DTE_IR  (1ull << 61)
#define DTE_IW  (1ull << 62)
#define DTE_ROOT_MASK 0x000FFFFFFFFFF000ull
#define DTE_INTCTL_PASS (1ull << 60)           /* слово 2, биты 61:60: 01 - прерывания устройства проходят без переназначения */

void amdvi_dte_passthrough(uint64_t d[4]) { d[0] = DTE_V | DTE_TV | DTE_IR | DTE_IW; d[1] = 0; d[2] = DTE_INTCTL_PASS; d[3] = 0; }
void amdvi_dte_translated(uint64_t d[4], uint64_t root, uint16_t dom) {
    d[0] = DTE_V | DTE_TV | DTE_IR | DTE_IW | (4ull << 9) | (root & DTE_ROOT_MASK);   /* режим 4 - четыре уровня */
    d[1] = dom; d[2] = DTE_INTCTL_PASS; d[3] = 0;
}
void amdvi_dte_blocked(uint64_t d[4]) { d[0] = d[1] = d[2] = d[3] = 0; }

/* ---- страницы трансляции ---- */
#define PTE_PR (1ull << 0)
#define PTE_IR (1ull << 61)
#define PTE_IW (1ull << 62)
#define PTE_ADDR 0x000FFFFFFFFFF000ull
static unsigned idx(uint64_t iova, int level) { return (unsigned)((iova >> (12 + 9 * (level - 1))) & 511); }
static int next_level(uint64_t e) { return (int)((e >> 9) & 7); }

int amdvi_pt_map(uint64_t* root, uint64_t iova, uint64_t phys, unsigned rw, const amdvi_mem_t* m) {
    if (!root || !m || (iova | phys) & 0xFFF || iova >> AMDVI_IOVA_BITS || phys >> 52 || !(rw & (AMDVI_MAP_R | AMDVI_MAP_W)) || rw & ~3u) return AMDVI_E_ARG;
    uint64_t* t = root;
    for (int level = 4; level > 1; level--) {
        uint64_t* e = &t[idx(iova, level)];
        if (!(*e & PTE_PR)) {
            uint64_t pa; void* v = m->alloc(m->ctx, &pa);
            if (!v || !pa || pa & 0xFFF) { if (v) m->release(m->ctx, v, pa); return AMDVI_E_NOMEM; }
            *e = (pa & PTE_ADDR) | PTE_PR | PTE_IR | PTE_IW | ((uint64_t)(level - 1) << 9);   /* права сверху полные, ограничивает лист */
        } else if (next_level(*e) != level - 1) return AMDVI_E_ARG;                            /* крупные страницы здесь не создаются и не принимаются */
        t = (uint64_t*)m->virt(m->ctx, *e & PTE_ADDR);
    }
    uint64_t* e = &t[idx(iova, 1)];
    if (*e & PTE_PR) return AMDVI_E_EXIST;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    *e = (phys & PTE_ADDR) | PTE_PR | (rw & AMDVI_MAP_R ? PTE_IR : 0) | (rw & AMDVI_MAP_W ? PTE_IW : 0);
    return 0;
}

static uint64_t* leaf_of(uint64_t* root, uint64_t iova, const amdvi_mem_t* m) {
    if (!root || iova >> AMDVI_IOVA_BITS) return 0;
    uint64_t* t = root;
    for (int level = 4; level > 1; level--) {
        uint64_t e = t[idx(iova, level)];
        if (!(e & PTE_PR) || next_level(e) != level - 1) return 0;
        t = (uint64_t*)m->virt(m->ctx, e & PTE_ADDR);
    }
    return &t[idx(iova, 1)];
}
int amdvi_pt_unmap(uint64_t* root, uint64_t iova, const amdvi_mem_t* m) {
    if (iova & 0xFFF) return AMDVI_E_ARG;
    uint64_t* e = leaf_of(root, iova, m);
    if (!e || !(*e & PTE_PR)) return AMDVI_E_NOENT;
    *e = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}
int amdvi_pt_lookup(uint64_t* root, uint64_t iova, uint64_t* phys, unsigned* rw, const amdvi_mem_t* m) {
    uint64_t* e = leaf_of(root, iova & ~0xFFFull, m);
    if (!e || !(*e & PTE_PR)) return AMDVI_E_NOENT;
    if (phys) *phys = *e & PTE_ADDR;
    if (rw) *rw = (*e & PTE_IR ? AMDVI_MAP_R : 0) | (*e & PTE_IW ? AMDVI_MAP_W : 0);
    return 0;
}
static void free_level(uint64_t* t, int level, const amdvi_mem_t* m) {
    if (level > 1)
        for (unsigned k = 0; k < 512; k++)
            if (t[k] & PTE_PR) {
                uint64_t pa = t[k] & PTE_ADDR;
                void* v = m->virt(m->ctx, pa);
                free_level((uint64_t*)v, level - 1, m);
                m->release(m->ctx, v, pa);
            }
}
void amdvi_pt_free(uint64_t* root, uint64_t root_phys, const amdvi_mem_t* m) {
    if (!root) return;
    free_level(root, 4, m);
    m->release(m->ctx, root, root_phys);
}

/* ---- команды ---- */
enum { CMD_COMPL_WAIT = 1, CMD_INV_DEVTAB = 2, CMD_INV_PAGES = 3 };
static void cmd_zero(uint32_t c[4]) { c[0] = c[1] = c[2] = c[3] = 0; }
void amdvi_cmd_completion_wait(uint32_t c[4], uint64_t phys, uint64_t data) {
    c[0] = (uint32_t)phys | 1u;                    /* S: записать данные по адресу */
    c[1] = (uint32_t)(phys >> 32) | (uint32_t)CMD_COMPL_WAIT << 28;
    c[2] = (uint32_t)data; c[3] = (uint32_t)(data >> 32);
}
void amdvi_cmd_inv_devtab(uint32_t c[4], uint16_t devid) { cmd_zero(c); c[0] = devid; c[1] = (uint32_t)CMD_INV_DEVTAB << 28; }
void amdvi_cmd_inv_pages_all(uint32_t c[4], uint16_t dom) {
    cmd_zero(c);
    c[1] = dom | (uint32_t)CMD_INV_PAGES << 28;
    c[2] = 0xFFFFF000u | 3u;                       /* адрес 0x7FFF...F000, S (весь диапазон) и PDE */
    c[3] = 0x7FFFFFFFu;
}
