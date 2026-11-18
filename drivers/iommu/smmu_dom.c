/* ARM SMMUv3: чистая логика (см. smmu_dom.h). Форматы - «Arm System Memory Management Unit Architecture Specification, SMMUv3». */
#include "smmu_dom.h"

#define STE_V       (1ull << 0)
#define STE_CFG(x)  ((uint64_t)(x) << 1)
enum { CFG_ABORT = 0, CFG_BYPASS = 4, CFG_S1 = 5 };

void smmu_ste_abort(uint64_t s[8]) { for (int i = 0; i < 8; i++) s[i] = 0; s[0] = STE_V | STE_CFG(CFG_ABORT); }   /* V=1 и Config=abort: запись действительна, DMA отвергается */
void smmu_ste_bypass(uint64_t s[8]) {
    for (int i = 0; i < 8; i++) s[i] = 0;
    s[1] = 1ull << 44;                                /* SHCFG: общий доступ как у запроса устройства */
    s[0] = STE_V | STE_CFG(CFG_BYPASS);
}
int smmu_ste_s1(uint64_t s[8], uint64_t cd) {
    if ((cd & 0x3F) || cd >> 52) return SMMU_D_ARG;
    for (int i = 0; i < 8; i++) s[i] = 0;
    /* S1DSS=2: запросы без номера подпотока идут через CD[0]; S1CIR/COR=1 и S1CSH=3: дескриптор читается как write-back, общий; SHCFG: как у запроса */
    s[1] = 2ull | 1ull << 2 | 1ull << 4 | 3ull << 6 | 1ull << 44;
    s[0] = STE_V | STE_CFG(CFG_S1) | (cd & 0x000FFFFFFFFFFFC0ull);       /* S1Fmt=0 (линейный), S1CDMax=0 (один контекст) */
    return 0;
}
int smmu_cd_s1(uint64_t c[8], uint64_t root, uint16_t asid, unsigned oas) {
    if ((root & 0xFFF) || root >> 48 || oas > 5) return SMMU_D_ARG;
    for (int i = 0; i < 8; i++) c[i] = 0;
    c[1] = root & 0x000FFFFFFFFFFFF0ull;                       /* TTB0 */
    c[3] = 0xFFull << 8 | 0x04ull;                             /* MAIR: атрибут 0 - устройство nGnRE (окна MMIO, например дверной звонок MSI), атрибут 1 - обычная память, write-back */
    c[0] = 16ull                                               /* T0SZ: вход 48 бит */
         | 1ull << 8 | 1ull << 10 | 3ull << 12                 /* таблицы: write-back, общие */
         | 1ull << 30                                          /* EPD1: вторая таблица не используется */
         | 1ull << 31                                          /* V */
         | (uint64_t)oas << 32                                 /* IPS */
         | 1ull << 41                                          /* AA64 */
         | 1ull << 45 | 1ull << 46 | 1ull << 47                /* R, A, ASET (как у Linux) */
         | (uint64_t)asid << 48;
    return 0;
}

/* ---- таблицы страниц, первая стадия ---- */
#define D_VALID 1ull
#define D_TABLE 2ull                                 /* биты 1:0 = 11: на уровнях 0-2 - таблица, на уровне 3 - страница */
#define D_ADDR  0x0000FFFFFFFFF000ull
#define D_ATTR1 (1ull << 2)                          /* AttrIndx 1 = MAIR[1] = обычная память write-back */
#define D_ATTR0 0ull                                 /* AttrIndx 0 = MAIR[0] = устройство nGnRE */
#define D_SH_OS (2ull << 8)
#define D_AP_USER (1ull << 6)                        /* AP[1]: доступ без привилегий - так ходят запросы устройств */
#define D_AP_RO (1ull << 7)                          /* AP[2]: только чтение */
#define D_SH_IS (3ull << 8)
#define D_AF    (1ull << 10)
#define D_NG    (1ull << 11)
#define D_XN    ((1ull << 53) | (1ull << 54))        /* PXN и UXN: исполнять из DMA-страниц нельзя никогда */
static unsigned idx(uint64_t ipa, int level) { return (unsigned)((ipa >> (12 + 9 * (3 - level))) & 511); }
static const unsigned oas_bits[6] = { 32, 36, 40, 42, 44, 48 };

int smmu_pt_map(uint64_t* root, uint64_t ipa, uint64_t phys, unsigned rw, unsigned oas, const smmu_mem_t* m) {
    if (!root || !m || ((ipa | phys) & 0xFFF) || ipa >> SMMU_IPA_BITS || oas > 5 || phys >> oas_bits[oas] || !(rw & 3) || rw & ~7u) return SMMU_D_ARG;
    uint64_t* t = root;
    for (int level = 0; level < 3; level++) {
        uint64_t* e = &t[idx(ipa, level)];
        if (!(*e & D_VALID)) {
            uint64_t pa; void* v = m->alloc(m->ctx, &pa);
            if (!v || !pa || (pa & 0xFFF)) { if (v) m->release(m->ctx, v, pa); return SMMU_D_NOMEM; }
            *e = (pa & D_ADDR) | D_TABLE | D_VALID;
        } else if (!(*e & D_TABLE)) return SMMU_D_ARG;                    /* блочные записи здесь не создаются */
        t = (uint64_t*)m->virt(m->ctx, *e & D_ADDR);
    }
    uint64_t* e = &t[idx(ipa, 3)];
    if (*e & D_VALID) return SMMU_D_EXIST;
    uint64_t ap = D_AP_USER | (rw & SMMU_MAP_W ? 0 : D_AP_RO);                      /* запись разрешена только с MAP_W; чтение есть всегда */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    uint64_t mem = rw & SMMU_MAP_DEV ? (D_ATTR0 | D_SH_OS) : (D_ATTR1 | D_SH_IS);      /* окно MMIO - как устройство, остальное - обычная память */
    *e = (phys & D_ADDR) | D_VALID | D_TABLE | mem | ap | D_AF | D_NG | D_XN;
    return 0;
}
static uint64_t* leaf_of(uint64_t* root, uint64_t ipa, const smmu_mem_t* m) {
    if (!root || ipa >> SMMU_IPA_BITS) return 0;
    uint64_t* t = root;
    for (int level = 0; level < 3; level++) {
        uint64_t e = t[idx(ipa, level)];
        if (!(e & D_VALID) || !(e & D_TABLE)) return 0;
        t = (uint64_t*)m->virt(m->ctx, e & D_ADDR);
    }
    return &t[idx(ipa, 3)];
}
int smmu_pt_unmap(uint64_t* root, uint64_t ipa, const smmu_mem_t* m) {
    if (ipa & 0xFFF) return SMMU_D_ARG;
    uint64_t* e = leaf_of(root, ipa, m);
    if (!e || !(*e & D_VALID)) return SMMU_D_NOENT;
    *e = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return 0;
}
int smmu_pt_lookup(uint64_t* root, uint64_t ipa, uint64_t* phys, unsigned* rw, const smmu_mem_t* m) {
    uint64_t* e = leaf_of(root, ipa & ~0xFFFull, m);
    if (!e || !(*e & D_VALID)) return SMMU_D_NOENT;
    if (phys) *phys = *e & D_ADDR;
    if (rw) *rw = SMMU_MAP_R | (*e & D_AP_RO ? 0 : SMMU_MAP_W);
    return 0;
}
static void free_level(uint64_t* t, int level, const smmu_mem_t* m) {
    if (level < 3)
        for (unsigned k = 0; k < 512; k++)
            if (t[k] & D_VALID) {
                uint64_t pa = t[k] & D_ADDR; void* v = m->virt(m->ctx, pa);
                free_level((uint64_t*)v, level + 1, m);
                m->release(m->ctx, v, pa);
            }
}
void smmu_pt_free(uint64_t* root, uint64_t root_phys, const smmu_mem_t* m) {
    if (!root) return;
    free_level(root, 0, m);
    m->release(m->ctx, root, root_phys);
}

/* ---- команды ---- */
enum { OP_CFGI_STE = 0x03, OP_CFGI_CD_ALL = 0x06, OP_TLBI_NH_ALL = 0x10, OP_SYNC = 0x46 };
void smmu_cmd_cfgi_ste(uint64_t c[2], uint32_t sid) { c[0] = OP_CFGI_STE | (uint64_t)sid << 32; c[1] = 1; /* Leaf: только эта запись */ }
void smmu_cmd_cfgi_cd_all(uint64_t c[2], uint32_t sid) { c[0] = OP_CFGI_CD_ALL | (uint64_t)sid << 32; c[1] = 0; }
void smmu_cmd_tlbi_nh_all(uint64_t c[2]) { c[0] = OP_TLBI_NH_ALL; c[1] = 0; }
void smmu_cmd_sync(uint64_t c[2]) { c[0] = OP_SYNC; c[1] = 0; }

/* ---- события ---- */
void smmu_event_decode(const uint64_t e[4], smmu_event_t* o) {
    o->id = (uint8_t)(e[0] & 0xFF);
    o->sid = (uint32_t)(e[0] >> 32);
    o->addr = e[2];
    o->is_write = !((e[1] >> 35) & 1);                 /* RnW: 1 - чтение */
}
const char* smmu_event_name(uint8_t id) {
    switch (id) {
    case 0x02: return "C_BAD_STREAMID"; case 0x03: return "F_STE_FETCH"; case 0x04: return "C_BAD_STE";
    case 0x06: return "F_STREAM_DISABLED"; case 0x07: return "F_TRANSL_FORBIDDEN"; case 0x0B: return "F_WALK_EABT";
    case 0x10: return "F_TRANSLATION"; case 0x11: return "F_ADDR_SIZE"; case 0x12: return "F_ACCESS"; case 0x13: return "F_PERMISSION";
    default: return "event";
    }
}
