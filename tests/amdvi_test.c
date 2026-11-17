/* AMD-Vi, чистая логика: разбор IVRS (в т.ч. фаззинг), DTE, страницы трансляции v1, команды. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/iommu/amd_vi.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void w16(uint8_t* p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void w32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static void w64(uint8_t* p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }
static size_t fix(uint8_t* t, size_t n) { w32(t + 4, (uint32_t)n); t[9] = 0; uint8_t s = 0; for (size_t i = 0; i < n; i++) s += t[i]; t[9] = (uint8_t)-s; return n; }

/* IVRS: один IVHD 0x11 (все устройства, одно с псевдонимом, IOAPIC) и IVMD для устройств 0x0010..0x0018. */
static size_t build(uint8_t* t, int type, uint64_t base) {
    memset(t, 0, 512); memcpy(t, "IVRS", 4); t[8] = 2; memcpy(t + 10, "KIRA  ", 6);
    w32(t + 36, 0x00203040);
    size_t p = 48, b = p;
    size_t hs = type == 0x10 ? 24 : 40;
    t[p] = (uint8_t)type; w16(t + p + 4, 0x0008); w16(t + p + 6, 0x40); w64(t + p + 8, base); w16(t + p + 16, 0);
    if (type != 0x10) w64(t + p + 24, 0x246);
    p += hs;
    t[p] = 3; w16(t + p + 1, 0x0020); p += 4;                       /* диапазон 0x20..0x2F */
    t[p] = 4; w16(t + p + 1, 0x002F); p += 4;
    t[p] = 2; w16(t + p + 1, 0x0030); p += 4;                       /* одно устройство */
    t[p] = 0x42; w16(t + p + 1, 0x0040); w16(t + p + 5, 0x0041); p += 8;   /* с псевдонимом */
    t[p] = 0x48; t[p + 3] = 2; w16(t + p + 4, 0x00A0); t[p + 6] = 1; p += 8;   /* IOAPIC */
    w16(t + b + 2, (uint16_t)(p - b));
    size_t m = p;
    t[p] = 0x22; t[p + 1] = 0x0B; w16(t + p + 2, 32); w16(t + p + 4, 0x0010); w16(t + p + 6, 0x0018); w64(t + p + 16, 0xE0000); w64(t + p + 24, 0x1000); p += 32;
    (void)m;
    return fix(t, p);
}

typedef struct { uint64_t pa[4096]; void* v[4096]; unsigned n; unsigned live; unsigned limit; } pool_t;
static void* palloc(void* c, uint64_t* pa) {
    pool_t* p = c;
    if (p->n >= p->limit) { *pa = 0; return 0; }
    void* v = aligned_alloc(4096, 4096); memset(v, 0, 4096);
    p->v[p->n] = v; p->pa[p->n] = 0x100000ull + (uint64_t)p->n * 4096; *pa = p->pa[p->n]; p->n++; p->live++; return v;
}
static void* pvirt(void* c, uint64_t pa) { pool_t* p = c; return p->v[(pa - 0x100000ull) / 4096]; }
static void prel(void* c, void* v, uint64_t pa) { pool_t* p = c; (void)pa; p->live--; unsigned i = (unsigned)((pa - 0x100000ull) / 4096); free(v); p->v[i] = 0; }

int main(void) {
    uint8_t t[512]; amdvi_info_t in;
    for (int ty = 0; ty < 3; ty++) {
        int type = ty == 0 ? 0x10 : ty == 1 ? 0x11 : 0x40;
        size_t n = build(t, type, 0xFED80000ull);
        CHECK(amdvi_ivrs_parse(t, n, &in) == 0);
        CHECK(in.nunits == 1 && in.unit[0].base == 0xFED80000ull && in.unit[0].devid == 8 && in.unit[0].type == type);
        CHECK(in.unit[0].efr == (type == 0x10 ? 0 : 0x246));
        CHECK(amdvi_unit_of(&in, 0x0025) == 0 && amdvi_unit_of(&in, 0x0030) == 0);
        CHECK(amdvi_unit_of(&in, 0x0021 + 0x20) == -1 && amdvi_unit_of(&in, 0x0010) == -1);       /* вне записей */
        CHECK(amdvi_unit_of(&in, 0x0040) == -2);                                                   /* только псевдоним */
        CHECK(in.nspecial == 1 && amdvi_is_special(&in, 0x00A0) && !amdvi_is_special(&in, 0x00A1));
        CHECK(in.nivmd == 1 && amdvi_ivmd_hit(&in, 0x0010) && amdvi_ivmd_hit(&in, 0x0018) && !amdvi_ivmd_hit(&in, 0x0019));
    }
    /* Отказы: контрольная сумма, подпись, обрезание, неверная база, диапазон без конца, нет блоков. */
    size_t n = build(t, 0x11, 0xFED80000ull);
    uint8_t bad[512]; memcpy(bad, t, n); bad[60] ^= 1;
    CHECK(amdvi_ivrs_parse(bad, n, &in) == AMDVI_E_FORMAT);
    memcpy(bad, t, n); bad[0] = 'X'; fix(bad, n); CHECK(amdvi_ivrs_parse(bad, n, &in) == AMDVI_E_FORMAT);
    CHECK(amdvi_ivrs_parse(t, n - 1, &in) == AMDVI_E_SHORT && amdvi_ivrs_parse(t, 20, &in) == AMDVI_E_SHORT && amdvi_ivrs_parse(0, 100, &in) == AMDVI_E_ARG);
    n = build(bad, 0x11, 0xFED80010ull); CHECK(amdvi_ivrs_parse(bad, n, &in) == AMDVI_E_FORMAT);
    n = build(bad, 0x11, 0); CHECK(amdvi_ivrs_parse(bad, n, &in) == AMDVI_E_FORMAT);
    n = build(bad, 0x11, 0xFED80000ull); bad[48 + 40 + 4] = 3; fix(bad, n); CHECK(amdvi_ivrs_parse(bad, n, &in) == AMDVI_E_FORMAT);   /* конец диапазона заменён началом */
    n = build(bad, 0x11, 0xFED80000ull); w16(bad + 48 + 16, 1); fix(bad, n); CHECK(amdvi_ivrs_parse(bad, n, &in) == AMDVI_E_NOUNIT);     /* сегмент 1 не поддержан */
    /* Фаззинг: любое искажение не должно падать (ASan/UBSan), результат либо успех, либо код ошибки. */
    srand(7); unsigned okc = 0, errc = 0;
    n = build(t, 0x11, 0xFED80000ull);
    for (int it = 0; it < 20000; it++) {
        memcpy(bad, t, n);
        int k = 1 + rand() % 4;
        while (k--) bad[rand() % n] = (uint8_t)rand();
        if (rand() & 1) fix(bad, n);
        size_t len = rand() % 8 ? n : (size_t)(rand() % n);
        int rc = amdvi_ivrs_parse(bad, len, &in);
        if (rc == 0) { okc++; for (unsigned r = 0; r < in.nranges; r++) CHECK(in.range[r].lo <= in.range[r].hi && in.range[r].unit < in.nunits); } else errc++;
    }
    CHECK(okc > 0 && errc > 0);

    /* DTE */
    uint64_t d[4];
    amdvi_dte_passthrough(d); CHECK((d[0] & 3) == 3 && !((d[0] >> 9) & 7) && (d[0] >> 61 & 3) == 3 && (d[2] >> 60 & 3) == 1);
    amdvi_dte_translated(d, 0x123456000ull, 77); CHECK((d[0] & 3) == 3 && ((d[0] >> 9) & 7) == 4 && (d[0] & 0x000FFFFFFFFFF000ull) == 0x123456000ull && d[1] == 77);
    amdvi_dte_blocked(d); CHECK(!d[0] && !d[1] && !d[2] && !d[3]);

    /* страницы */
    pool_t pool = { .limit = 4000 }; amdvi_mem_t m = { &pool, palloc, prel, pvirt };
    uint64_t rp; uint64_t* root = palloc(&pool, &rp);
    CHECK(amdvi_pt_map(root, 0x7000, 0xABC000, AMDVI_MAP_R | AMDVI_MAP_W, &m) == 0);
    uint64_t ph; unsigned rw;
    CHECK(amdvi_pt_lookup(root, 0x7123, &ph, &rw, &m) == 0 && ph == 0xABC000 && rw == 3);
    CHECK(amdvi_pt_map(root, 0x8000, 0xDEF000, AMDVI_MAP_R, &m) == 0 && amdvi_pt_lookup(root, 0x8000, &ph, &rw, &m) == 0 && rw == AMDVI_MAP_R);
    CHECK(amdvi_pt_map(root, 0x7000, 0x1000, AMDVI_MAP_R, &m) == AMDVI_E_EXIST);
    CHECK(amdvi_pt_map(root, 0x7001, 0x1000, 1, &m) == AMDVI_E_ARG && amdvi_pt_map(root, 0x9000, 0x1000, 0, &m) == AMDVI_E_ARG);
    CHECK(amdvi_pt_map(root, 1ull << 48, 0x1000, 1, &m) == AMDVI_E_ARG && amdvi_pt_map(root, 0xA000, 1ull << 52, 1, &m) == AMDVI_E_ARG);
    CHECK(amdvi_pt_map(root, 0x7FFFFFFFF000ull, 0x5000, 3, &m) == 0 && amdvi_pt_lookup(root, 0x7FFFFFFFF000ull, &ph, 0, &m) == 0 && ph == 0x5000);
    CHECK(amdvi_pt_unmap(root, 0x7000, &m) == 0 && amdvi_pt_lookup(root, 0x7000, &ph, &rw, &m) == AMDVI_E_NOENT);
    CHECK(amdvi_pt_unmap(root, 0x7000, &m) == AMDVI_E_NOENT && amdvi_pt_unmap(root, 0x123000000ull, &m) == AMDVI_E_NOENT);
    CHECK(amdvi_pt_lookup(root, 0x8000, &ph, &rw, &m) == 0);                 /* соседняя страница цела */
    unsigned live_before = pool.live; CHECK(live_before > 1);
    amdvi_pt_free(root, rp, &m); CHECK(pool.live == 0);                      /* всё освобождено, включая корень */
    /* Нехватка памяти: отображение отказывает, уже созданные таблицы не теряются (освобождаются при удалении дерева). */
    pool.n = 0; pool.live = 0; pool.limit = 3; root = palloc(&pool, &rp);
    CHECK(amdvi_pt_map(root, 0x1000, 0x2000, 3, &m) == AMDVI_E_NOMEM);
    amdvi_pt_free(root, rp, &m); CHECK(pool.live == 0);
    /* команды */
    uint32_t c[4];
    amdvi_cmd_completion_wait(c, 0x123456788ull, 0x1122334455667788ull);
    CHECK(c[0] == (0x23456788u | 1u) && (c[1] >> 28) == 1 && (c[1] & 0xFFFFF) == 1 && c[2] == 0x55667788u && c[3] == 0x11223344u);
    amdvi_cmd_inv_devtab(c, 0x1234); CHECK(c[0] == 0x1234 && (c[1] >> 28) == 2 && !c[2] && !c[3]);
    amdvi_cmd_inv_pages_all(c, 9); CHECK((c[1] >> 28) == 3 && (c[1] & 0xFFFF) == 9 && (c[2] & 3) == 3 && c[3] == 0x7FFFFFFFu);
    printf(fails ? "amdvi: FAILED (%d)\n" : "amdvi: IVRS parsing (+fuzz), DTE, v1 page tables, commands passed\n", fails);
    return fails != 0;
}
