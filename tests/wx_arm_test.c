// Хост-тест логики W^X для ARM64 (arch/aarch64/wx_arm.c): дробление блоков 1 ГБ / 2 МБ под образом, права секций,
// XN на остальной карте, обход таблиц с поиском записываемого+исполняемого, проверка раскладки образа.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "wx_arm.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define MB (1ULL << 20)
#define GB (1ULL << 30)
#define V 1ULL
#define PG 3ULL
#define AF (1ULL << 10)
#define SH (3ULL << 8)
#define AP2 (1ULL << 7)
#define PXN (1ULL << 53)
#define UXN (1ULL << 54)
#define NORMAL ((2ULL << 2) | AF | SH)
#define DEVICE ((1ULL << 2) | AF | PXN | UXN)
#define OA 0x0000FFFFFFFFF000ULL
static int fails, budget;

static uint64_t* ptr(uint64_t p) { return (uint64_t*)(uintptr_t)p; }
static uint64_t newpage(void) { void* p = aligned_alloc(4096, 4096); memset(p, 0, 4096); return (uint64_t)(uintptr_t)p; }
static uint64_t alloc_limited(void) { if (budget <= 0) return 0; budget--; return newpage(); }
static const wx_env_t env = { ptr, alloc_limited };

// Образ в ОЗУ с 0x40000000: текст 0x40080000..0x40180000 (256 стр.), чтение ..0x401a0000 (32), данные ..0x40400000 (608).
static const wx_layout_t L = { 0x40080000, 0x40180000, 0x40180000, 0x401a0000, 0x401a0000, 0x40400000, 0, 0 };
static uint64_t* l0;
static uint64_t* l1;

static void build(int ram_blocks_2m) {
    l0 = ptr(newpage()); l1 = ptr(newpage());
    l0[0] = (uint64_t)(uintptr_t)l1 | PG;
    l1[0] = 0 | DEVICE | V;                               // регистры устройств: блок 1 ГБ
    if (!ram_blocks_2m) l1[1] = GB | NORMAL | V;          // ОЗУ целым блоком 1 ГБ (без пула DMA)
    else {
        uint64_t* l2 = ptr(newpage());
        l1[1] = (uint64_t)(uintptr_t)l2 | PG;
        for (unsigned i = 0; i < 512; i++) l2[i] = (GB + (uint64_t)i * 2 * MB) | NORMAL | V;
    }
    l1[2] = 2 * GB | NORMAL | V;                          // второй гигабайт ОЗУ, образа в нём нет
}

int main(void) {
    wx_stats_t st;
    for (int variant = 0; variant < 2; variant++) {
        budget = 100; build(variant);
        CHECK(wxa_scan(&env, l0, &L, &st) == -1 && st.bad_count > 0 && !st.layout_ok);   // исходная карта: ОЗУ RW без PXN
        budget = 100;
        CHECK(wxa_enforce(&env, l0, &L) == 0);
        CHECK(wxa_scan(&env, l0, &L, &st) == 0 && st.bad_count == 0 && st.layout_ok);
        CHECK(st.exec_kb == 1024);                                                          // 256 стр. текста
        CHECK(st.ro_kb == 128);                                                             // 32 стр. данных для чтения
        CHECK(st.rw_kb == 2 * 1048576 - 1024 - 128);                                        // остальные 2 ГБ ОЗУ; устройства не считаются
        CHECK((l1[0] & PXN) && (l1[0] & UXN));
        CHECK((l1[2] & PXN) && (l1[2] & UXN) && !(l1[2] & AP2));                            // ОЗУ вне образа: RW, без исполнения
        uint64_t* t0 = ptr(ptr(l1[1] & OA)[0] & OA);                                        // 2-МБ область 0x40000000: страницы
        CHECK(t0[0] == (0x40000000ULL | PG | NORMAL | PXN | UXN));                          // до образа: RW без исполнения
        CHECK(t0[0x80] == (0x40080000ULL | PG | NORMAL | AP2 | UXN));                 // первая страница текста: RO, PXN снят
        CHECK(t0[0x17f] == (0x4017f000ULL | PG | NORMAL | AP2 | UXN));                      // последняя страница текста
        CHECK(t0[0x180] == (0x40180000ULL | PG | NORMAL | AP2 | PXN | UXN));                // первая страница чтения
        CHECK(t0[0x19f] == (0x4019f000ULL | PG | NORMAL | AP2 | PXN | UXN));
        CHECK(t0[0x1a0] == (0x401a0000ULL | PG | NORMAL | PXN | UXN));                      // первая страница данных: RW, PXN
        uint64_t* t1 = ptr(ptr(l1[1] & OA)[1] & OA);
        CHECK(t1[0] == (0x40200000ULL | PG | NORMAL | PXN | UXN));
        uint64_t* l2 = ptr(l1[1] & OA);
        CHECK(l2[100] == ((GB + 100 * 2 * MB) | NORMAL | V | PXN | UXN));                   // 2-МБ блок вне образа остался блоком
        CHECK((l2[0] & 3) == PG && (l2[1] & 3) == PG);
    }

    // Нарушения находятся.
    uint64_t* t0 = ptr(ptr(l1[1] & OA)[0] & OA);
    uint64_t save = t0[0x1a5];
    t0[0x1a5] &= ~PXN;
    CHECK(wxa_scan(&env, l0, &L, &st) == -1 && st.bad_count == 1 && st.bad_va == 0x401a5000 && !st.layout_ok);
    t0[0x1a5] = save;
    save = t0[0x90];
    t0[0x90] &= ~AP2;                                                                       // текст записываем
    CHECK(wxa_scan(&env, l0, &L, &st) == -1 && st.bad_count == 1 && st.bad_va == 0x40090000 && st.layout_bad_va == 0x40090000);
    t0[0x90] = save;
    save = t0[0x185];
    t0[0x185] &= ~AP2;                                                                      // чтение записываемо, без исполнения: раскладка нарушена
    CHECK(wxa_scan(&env, l0, &L, &st) == -1 && st.bad_count == 0 && st.layout_bad_va == 0x40185000);
    t0[0x185] = save;
    save = t0[0x1a1];
    t0[0x1a1] |= (1ULL << 6);                                                               // страница данных доступна из EL0 с записью, UXN есть - не нарушение
    CHECK(wxa_scan(&env, l0, &L, &st) == 0);
    t0[0x1a1] &= ~UXN;                                                                      // EL0: запись + исполнение
    CHECK(wxa_scan(&env, l0, &L, &st) == -1 && st.bad_count == 1 && st.bad_va == 0x401a1000);
    t0[0x1a1] = save;
    save = t0[0x100];
    t0[0x100] = 0;
    CHECK(wxa_scan(&env, l0, &L, &st) == -1 && st.layout_bad_va == 0x40100000);
    t0[0x100] = save;
    CHECK(wxa_scan(&env, l0, &L, &st) == 0);

    // Повторное применение идемпотентно.
    budget = 0;
    CHECK(wxa_enforce(&env, l0, &L) == 0 && wxa_scan(&env, l0, &L, &st) == 0);

    // Отказы: нет памяти; образ в устройстве; блок на уровне L0.
    budget = 100; build(0); budget = 1;
    CHECK(wxa_enforce(&env, l0, &L) == -1);
    budget = 100; build(0); l1[1] = GB | DEVICE | V;
    CHECK(wxa_enforce(&env, l0, &L) == -1);
    budget = 100; build(0); l0[5] = GB | NORMAL | V;
    CHECK(wxa_enforce(&env, l0, &L) == -1);

    printf(fails ? "wx_arm: FAIL (%d)\n" : "wx_arm: PASS\n", fails);
    return fails ? 1 : 0;
}
