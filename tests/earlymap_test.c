// Хост-тест ранней карты страниц образа (earlymap.c): права по секциям с первой инструкции (ни одной страницы W+X, это
// проверяет тот же wx_scan, что и работающее ядро), образ на старом (1 МБ) и на случайном месте, мост KASLR и его закрытие,
// нехватка пула, неверные границы.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "earlymap.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define MB (1ULL << 20)
#define ADDR 0x000FFFFFFFFFF000ULL
static int fails;

static uint64_t* ptr(uint64_t p) { return (uint64_t*)(uintptr_t)p; }
static const wx_env_t env = { ptr, 0 };

static uint8_t* arena;
static earlymap_pa_t pa;
static void fresh(unsigned pool_n) {
    free(arena);
    arena = aligned_alloc(4096, 4096 * (3 + EARLYMAP_PT_MAX));
    memset(arena, 0xA5, 4096 * (3 + EARLYMAP_PT_MAX));                // мусор: build обязан задать всё сам
    pa.pml4 = (uint64_t)(uintptr_t)arena; pa.pdpt = pa.pml4 + 4096; pa.pd = pa.pml4 + 8192;
    pa.pt_pool = pa.pml4 + 12288; pa.pt_pool_n = pool_n;
}

// Как в build_clang.sh/linker.ld: rodata 1 МБ + 0x26000, text до +0x110000, data+bss до +0x4C0000 (около 5 МБ образа).
static earlymap_layout_t layout_at(uint64_t base, uint64_t bridge) {
    earlymap_layout_t l;
    l.w.ro_lo = base;                  l.w.ro_hi = base + 0x26000;
    l.w.text_lo = base + 0x26000;      l.w.text_hi = base + 0x110000;
    l.w.data_lo = base + 0x110000;     l.w.data_hi = base + 0x4C0000;
    l.w.exec_lo = 0x8000;              l.w.exec_hi = 0x9000;
    l.image_lo = base;                 l.image_hi = base + 0x4C0000;
    l.exec2_lo = bridge;               l.exec2_hi = bridge ? bridge + 4096 : 0;
    return l;
}

static int leaf(uint64_t va, int* w, int* nx, int* huge) {
    const uint64_t* pml4 = ptr(pa.pml4);
    const uint64_t* pdpt = ptr(pml4[0] & ADDR);
    const uint64_t* pd = ptr(pdpt[0] & ADDR);
    uint64_t e = pd[(va >> 21) & 0x1FF];
    if (!(e & WX_P)) return -1;
    if (e & WX_PS) { *w = !!(e & WX_W); *nx = !!(e & WX_NX); *huge = 1; return 0; }
    const uint64_t* pt = ptr(e & ADDR);
    e = pt[(va >> 12) & 0x1FF];
    if (!(e & WX_P)) return -1;
    *w = !!(e & WX_W); *nx = !!(e & WX_NX); *huge = 0;
    return 0;
}

static void check_rights(const earlymap_layout_t* l) {
    wx_stats_t st;
    CHECK(wx_scan(&env, ptr(pa.pml4), &l->w, &st) == 0);              // нет W+X, образ по секциям
    CHECK(st.bad_count == 0 && st.layout_ok);
    int w, nx, huge;
    // текст - RX, rodata - R+NX, data - RW+NX, трамплин - RX, обычная память - RW+NX
    CHECK(leaf(l->w.text_lo, &w, &nx, &huge) == 0 && !w && !nx && !huge);
    CHECK(leaf(l->w.text_hi - 4096, &w, &nx, &huge) == 0 && !w && !nx);
    CHECK(leaf(l->w.ro_lo, &w, &nx, &huge) == 0 && !w && nx);
    CHECK(leaf(l->w.data_lo, &w, &nx, &huge) == 0 && w && nx);
    CHECK(leaf(l->w.data_hi - 4096, &w, &nx, &huge) == 0 && w && nx);
    CHECK(leaf(0x8000, &w, &nx, &huge) == 0 && !w && !nx);           // трамплин AP исполняем, но не записываем
    CHECK(leaf(0x9000, &w, &nx, &huge) == 0 && w && nx);              // соседняя страница - данные
    CHECK(leaf(0xB8000, &w, &nx, &huge) == 0 && w && nx);             // экран VGA
    CHECK(leaf(0x0, &w, &nx, &huge) == 0 && w && nx);                 // страницу 0 снимает vmm_guard_null_page позже
    CHECK(leaf(768 * MB, &w, &nx, &huge) == 0 && w && nx && huge);    // вся прочая память - 2-МБ страницы RW+NX
    if (l->image_hi <= 1022 * MB) CHECK(leaf(1023 * MB, &w, &nx, &huge) == 0 && w && nx && huge);
    CHECK(ptr(ptr(pa.pml4)[0] & ADDR)[1] == 0);                       // карта - ровно первый гигабайт
    // сводка: исполняемого немного (текст + трамплин), больше всего RW+NX
    CHECK(st.exec_kb == (l->w.text_hi - l->w.text_lo) / 1024 + 4 + (l->exec2_hi ? 4 : 0));
}

static void test_old_place(void) {                                    // образ на месте загрузки (boot.s делает то же на ассемблере)
    fresh(EARLYMAP_PT_MAX);
    earlymap_layout_t l = layout_at(1 * MB, 0);
    CHECK(earlymap_build(&env, &pa, &l) == 0);
    check_rights(&l);
    // области 0..2 (образ 1..6 МБ) раздроблены, четвёртая - уже 2-МБ страница
    int w, nx, huge;
    CHECK(leaf(5 * MB, &w, &nx, &huge) == 0 && !huge);
    CHECK(leaf(6 * MB, &w, &nx, &huge) == 0 && huge);
}

static void test_random_place(void) {                                 // KASLR: образ на 2-МБ границе, мост - страница старого образа
    for (uint64_t base = 16 * MB; base < 1024 * MB; base += 114 * MB) {
        base &= ~(2 * MB - 1);
        fresh(EARLYMAP_PT_MAX);
        earlymap_layout_t l = layout_at(base, 0x140000);              // мост в старом образе
        CHECK(earlymap_build(&env, &pa, &l) == 0);
        check_rights(&l);
        int w, nx, huge;
        CHECK(leaf(0x140000, &w, &nx, &huge) == 0 && !w && !nx);      // мост исполняем (и не записываем) в обеих картах
        CHECK(leaf(0x141000, &w, &nx, &huge) == 0 && w && nx);        // а остальной старый образ - данные
        CHECK(leaf(0x100000, &w, &nx, &huge) == 0 && w && nx);
        // закрытие моста
        CHECK(earlymap_make_data(&env, pa.pml4, 0x140000) == 0);
        CHECK(leaf(0x140000, &w, &nx, &huge) == 0 && w && nx);
        earlymap_layout_t l2 = l; l2.exec2_lo = l2.exec2_hi = 0;
        check_rights(&l2);                                            // после закрытия - как без моста
    }
    // образ вплотную к концу первого гигабайта
    fresh(EARLYMAP_PT_MAX);
    earlymap_layout_t e = layout_at(1024 * MB - 6 * MB, 0x140000);
    e.image_hi = 1024 * MB - 6 * MB + 0x4C0000;
    CHECK(earlymap_build(&env, &pa, &e) == 0);
    check_rights(&e);
}

static void test_errors(void) {
    earlymap_layout_t l = layout_at(1 * MB, 0x140000);
    fresh(2);                                                         // нужно 3 области образа + область 0... пул мал
    CHECK(earlymap_build(&env, &pa, &l) == -2);
    fresh(EARLYMAP_PT_MAX);
    l = layout_at(1 * MB, 0); l.w.text_lo += 8;                       // не выровнено
    CHECK(earlymap_build(&env, &pa, &l) == -1);
    l = layout_at(1 * MB, 0); l.image_hi = 1024 * MB + 4096;          // за пределом карты
    CHECK(earlymap_build(&env, &pa, &l) == -1);
    l = layout_at(1 * MB, 0); l.image_lo = l.image_hi;
    CHECK(earlymap_build(&env, &pa, &l) == -1);
    // make_data: 2-МБ страница, отсутствующая страница и адрес вне карты не трогаются
    l = layout_at(16 * MB, 0x140000);
    CHECK(earlymap_build(&env, &pa, &l) == 0);
    CHECK(earlymap_make_data(&env, pa.pml4, 700 * MB) == -1);         // 2-МБ страница
    CHECK(earlymap_make_data(&env, pa.pml4, 1024 * MB) == -1);
    CHECK(earlymap_make_data(&env, pa.pml4, 0x140100) == -1);         // не выровнен
    ptr(pa.pml4)[0] = 0;
    CHECK(earlymap_make_data(&env, pa.pml4, 0x140000) == -1);         // нет PML4[0]
}

// Старые образы (до этого этапа): 2-МБ RWX под образом - обход должен это видеть; контроль, что тест не пропускает нарушения.
static void test_scan_sees_violation(void) {
    fresh(EARLYMAP_PT_MAX);
    earlymap_layout_t l = layout_at(1 * MB, 0);
    CHECK(earlymap_build(&env, &pa, &l) == 0);
    int w, nx, huge;
    (void)w; (void)nx; (void)huge;
    uint64_t* pd = ptr(ptr(ptr(pa.pml4)[0] & ADDR)[0] & ADDR);
    pd[1] = (2 * MB) | WX_P | WX_W | WX_PS;                           // 2-МБ страница RWX, как было в boot.s
    wx_stats_t st;
    CHECK(wx_scan(&env, ptr(pa.pml4), &l.w, &st) == -1 && st.bad_count == 1 && !st.layout_ok);
}

int main(void) {
    test_old_place(); test_random_place(); test_errors(); test_scan_sees_violation();
    free(arena);
    if (fails) { printf("earlymap_test: %d FAILED\n", fails); return 1; }
    printf("earlymap_test: all passed\n");
    return 0;
}
