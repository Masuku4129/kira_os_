/* ARM SMMUv3, чистая логика: STE, таблицы страниц первой стадии, команды, события. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/iommu/smmu_dom.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
typedef struct { uint64_t pa[2048]; void* v[2048]; unsigned n, live, limit; } pool_t;
static void* palloc(void* c, uint64_t* pa) { pool_t* p = c; if (p->n >= p->limit) { *pa = 0; return 0; } void* v = aligned_alloc(4096, 4096); memset(v, 0, 4096); p->v[p->n] = v; p->pa[p->n] = 0x200000ull + (uint64_t)p->n * 4096; *pa = p->pa[p->n++]; p->live++; return v; }
static void* pvirt(void* c, uint64_t pa) { pool_t* p = c; return p->v[(pa - 0x200000ull) / 4096]; }
static void prel(void* c, void* v, uint64_t pa) { pool_t* p = c; (void)pa; p->live--; free(v); p->v[(pa - 0x200000ull) / 4096] = 0; }
int main(void) {
    uint64_t s[8];
    smmu_ste_abort(s); CHECK((s[0] & 1) && ((s[0] >> 1) & 7) == 0);
    smmu_ste_bypass(s); CHECK((s[0] & 1) && ((s[0] >> 1) & 7) == 4);
    CHECK(smmu_ste_s1(s, 0x40002000ull) == 0);
    CHECK((s[0] & 1) && ((s[0] >> 1) & 7) == 5 && (s[0] & 0x000FFFFFFFFFFFC0ull) == 0x40002000ull && (s[0] >> 4 & 3) == 0 && (s[0] >> 59) == 0);
    CHECK((s[1] & 3) == 2 && (s[1] >> 44 & 3) == 1);
    CHECK(smmu_ste_s1(s, 0x40002020ull) == SMMU_D_ARG && smmu_ste_s1(s, 1ull << 52) == SMMU_D_ARG);
    uint64_t cd[8];
    CHECK(smmu_cd_s1(cd, 0x40001000ull, 0x1234, 4) == 0);
    CHECK((cd[0] & 0x3F) == 16 && (cd[0] >> 6 & 3) == 0 && (cd[0] >> 31 & 1) && (cd[0] >> 30 & 1) && (cd[0] >> 32 & 7) == 4 && (cd[0] >> 41 & 1) && (cd[0] >> 48) == 0x1234);
    CHECK((cd[1] & 0x000FFFFFFFFFFFF0ull) == 0x40001000ull && cd[3] == 0xFF00ull && !cd[2]);
    CHECK(smmu_cd_s1(cd, 0x40001800ull, 1, 4) == SMMU_D_ARG && smmu_cd_s1(cd, 0x1000, 1, 6) == SMMU_D_ARG && smmu_cd_s1(cd, 1ull << 48, 1, 4) == SMMU_D_ARG);
    pool_t pool = { .limit = 2000 }; smmu_mem_t m = { &pool, palloc, prel, pvirt };
    uint64_t rp; uint64_t* root = palloc(&pool, &rp); uint64_t ph; unsigned rw;
    CHECK(smmu_pt_map(root, 0x7000, 0x80ABC000ull, SMMU_MAP_R | SMMU_MAP_W, 5, &m) == 0);
    CHECK(smmu_pt_lookup(root, 0x7123, &ph, &rw, &m) == 0 && ph == 0x80ABC000ull && rw == 3);
    CHECK(smmu_pt_map(root, 0x8000, 0x1000, SMMU_MAP_R, 5, &m) == 0 && smmu_pt_lookup(root, 0x8000, &ph, &rw, &m) == 0 && rw == 1);
    CHECK(smmu_pt_map(root, 0x9000, 0x2000, SMMU_MAP_W, 5, &m) == 0 && smmu_pt_lookup(root, 0x9000, &ph, &rw, &m) == 0 && rw == 3);   /* первая стадия: запись подразумевает чтение */
    CHECK(smmu_pt_map(root, 0x7000, 0x1000, 1, 5, &m) == SMMU_D_EXIST);
    CHECK(smmu_pt_map(root, 0x7001, 0x1000, 1, 5, &m) == SMMU_D_ARG && smmu_pt_map(root, 0xA000, 0x1000, 0, 5, &m) == SMMU_D_ARG);
    CHECK(smmu_pt_map(root, 1ull << 48, 0x1000, 1, 5, &m) == SMMU_D_ARG && smmu_pt_map(root, 0xA000, 1ull << 40, 1, 2, &m) == SMMU_D_ARG);   /* 40-битный выход не вмещает 2^40 */
    CHECK(smmu_pt_map(root, 0xA000, 1ull << 40, 1, 3, &m) == 0);                                                                                /* 42 бита вмещает */
    CHECK(smmu_pt_unmap(root, 0x7000, &m) == 0 && smmu_pt_lookup(root, 0x7000, &ph, &rw, &m) == SMMU_D_NOENT && smmu_pt_unmap(root, 0x7000, &m) == SMMU_D_NOENT);
    CHECK(smmu_pt_lookup(root, 0x8000, &ph, 0, &m) == 0);
    /* Дескриптор листа: атрибуты и запрет исполнения. */
    uint64_t* l0 = root; uint64_t* t = l0; for (int lv = 0; lv < 3; lv++) { uint64_t e = t[(0x8000ull >> (12 + 9 * (3 - lv))) & 511]; t = pvirt(&pool, e & 0x0000FFFFFFFFF000ull); }
    uint64_t leaf = t[8]; CHECK((leaf & 3) == 3 && (leaf >> 10 & 1) && (leaf >> 54 & 1) && (leaf >> 53 & 1) && (leaf >> 7 & 1) && (leaf >> 6 & 1) && (leaf >> 8 & 3) == 3 && (leaf >> 2 & 7) == 1 && (leaf >> 11 & 1));
    smmu_pt_free(root, rp, &m); CHECK(pool.live == 0);
    pool.n = 0; pool.live = 0; pool.limit = 2; root = palloc(&pool, &rp);
    CHECK(smmu_pt_map(root, 0x1000, 0x2000, 3, 5, &m) == SMMU_D_NOMEM); smmu_pt_free(root, rp, &m); CHECK(pool.live == 0);
    uint64_t c[2];
    smmu_cmd_cfgi_ste(c, 0x1234); CHECK((c[0] & 0xFF) == 0x03 && (c[0] >> 32) == 0x1234 && c[1] == 1);
    smmu_cmd_tlbi_nh_all(c); CHECK((c[0] & 0xFF) == 0x10);
    smmu_cmd_cfgi_cd_all(c, 7); CHECK((c[0] & 0xFF) == 0x06 && (c[0] >> 32) == 7);
    smmu_cmd_sync(c); CHECK((c[0] & 0xFF) == 0x46);
    uint64_t e[4] = { 0x10 | 5ull << 32, 0, 0xDEAD000ull, 0 }; smmu_event_t ev;
    smmu_event_decode(e, &ev); CHECK(ev.id == 0x10 && ev.sid == 5 && ev.addr == 0xDEAD000ull && ev.is_write);
    e[1] = 1ull << 35; smmu_event_decode(e, &ev); CHECK(!ev.is_write);
    CHECK(strcmp(smmu_event_name(0x10), "F_TRANSLATION") == 0 && strcmp(smmu_event_name(0xFF), "event") == 0);
    printf(fails ? "smmu_dom: FAILED (%d)\n" : "smmu_dom: STE, CD, stage-1 page tables, commands, events passed\n", fails);
    return fails != 0;
}
