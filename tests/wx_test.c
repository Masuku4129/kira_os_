// Хост-тест логики W^X (wx.c): дробление 2-МБ страниц под образом ядра, права по секциям, NX на прямой карте,
// обход таблиц с поиском W+X (эффективные права по всем уровням) и проверка раскладки образа.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "wx.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define MB (1ULL << 20)
#define GB (1ULL << 30)
#define ADDR 0x000FFFFFFFFFF000ULL
static int fails;
static int budget;

static uint64_t* ptr(uint64_t p) { return (uint64_t*)(uintptr_t)p; }
static uint64_t newpage(void) {
    void* p = aligned_alloc(4096, 4096);
    memset(p, 0, 4096);
    return (uint64_t)(uintptr_t)p;
}
static uint64_t alloc_limited(void) { if (budget <= 0) return 0; budget--; return newpage(); }
static const wx_env_t env = { ptr, alloc_limited };

// Таблицы как после boot.s + ramext + vmm_guard_null_page: первый гигабайт 2-МБ страницами RW, первая 2-МБ страница уже
// раздроблена (страница 0 снята), выше 4 ГБ - 2-МБ страницы RW+NX, плюс верхняя половина с окном MMIO.
static uint64_t* pml4;
static void build(void) {
    pml4 = ptr(newpage());
    uint64_t* pdpt = ptr(newpage());
    uint64_t* pd0 = ptr(newpage());
    uint64_t* pd4 = ptr(newpage());
    pml4[0] = (uint64_t)(uintptr_t)pdpt | 3;
    pdpt[0] = (uint64_t)(uintptr_t)pd0 | 3;
    pdpt[4] = (uint64_t)(uintptr_t)pd4 | 3;
    for (unsigned i = 0; i < 512; i++) { pd0[i] = ((uint64_t)i * 2 * MB) | 0x83; pd4[i] = (4 * GB + (uint64_t)i * 2 * MB) | 0x83 | WX_NX; }
    uint64_t* pt = ptr(newpage());
    for (unsigned i = 1; i < 512; i++) pt[i] = ((uint64_t)i * 4096) | 3;
    pd0[0] = (uint64_t)(uintptr_t)pt | 3;
    uint64_t* mpdpt = ptr(newpage());
    uint64_t* mpd = ptr(newpage());
    pml4[510] = (uint64_t)(uintptr_t)mpdpt | 3;
    mpdpt[0] = (uint64_t)(uintptr_t)mpd | 3;
    mpd[0] = (0xFEE00000ULL) | 0x83 | WX_NX | (1 << 4);
}
static uint64_t* pt_of(unsigned di) { uint64_t* pdpt = ptr(pml4[0] & ADDR); uint64_t* pd = ptr(pdpt[0] & ADDR); return ptr(pd[di] & ADDR); }
static uint64_t* pd_first(void) { uint64_t* pdpt = ptr(pml4[0] & ADDR); return ptr(pdpt[0] & ADDR); }

// Образ: rodata 0x100000..0x11d000 (29 стр.), text 0x11d000..0x1c9000 (172 стр.), data+bss 0x1c9000..0x5b0000 (487 стр.): три 2-МБ страницы.
static const wx_layout_t L = { 0x11d000, 0x1c9000, 0x100000, 0x11d000, 0x1c9000, 0x5b0000, 0x8000, 0x9000 };

int main(void) {
    budget = 100; build();
    wx_stats_t st;
    CHECK(wx_scan(&env, pml4, &L, &st) == -1);          // до enforce: каждая страница первого гигабайта W+X
    CHECK(st.bad_count > 500 && !st.layout_ok);

    budget = 100;
    CHECK(wx_enforce(&env, pml4, &L) == 0);
    CHECK(budget == 100 - 2);                           // раздроблены 2-МБ страницы 1 и 2; страница 0 была раздроблена раньше
    CHECK(wx_scan(&env, pml4, &L, &st) == 0);
    CHECK(st.bad_count == 0 && st.layout_ok);
    CHECK(st.exec_kb == (0x1c9000 - 0x11d000) / 1024 + 4);          // текст + страница трамплина
    CHECK(st.ro_kb == (0x11d000 - 0x100000) / 1024);                // только rodata (окно MMIO - RW+NX)
    uint64_t* pd = pd_first();
    CHECK((pd[10] & WX_NX) && (pd[10] & WX_PS) && (pd[10] & WX_W));  // прямая карта вне образа: RW+NX
    CHECK(!(pd[1] & WX_PS) && !(pd[2] & WX_PS));                      // под образом - 4-КБ таблицы
    uint64_t* pt0 = pt_of(0);
    CHECK(pt0[0] == 0);                                               // ловушка NULL сохранена
    CHECK(pt0[8] == (0x8000 | WX_P));                                 // трамплин: RX
    CHECK(pt0[9] == (0x9000 | 3 | WX_NX));                            // соседняя - RW+NX
    uint64_t* pt_a = pt_of(0);
    CHECK(pt_a[0x100] == (0x100000 | WX_P | WX_NX));                  // первая страница rodata: R+NX
    CHECK(pt_a[0x11c] == (0x11c000 | WX_P | WX_NX));                  // последняя страница rodata
    CHECK(pt_a[0x11d] == (0x11d000 | WX_P));                          // первая страница текста: RX
    CHECK(pt_a[0x1c8] == (0x1c8000 | WX_P));                          // последняя страница текста
    CHECK(pt_a[0x1c9] == (0x1c9000 | WX_P | WX_W | WX_NX));          // первая страница data: RW+NX
    uint64_t* pt_b = pt_of(1);
    CHECK(pt_b[0] == (2 * MB | 3 | WX_NX));
    uint64_t* pt_c = pt_of(2);
    CHECK(pt_c[0x1b0 - 0x00] == ((4 * MB + 0x1b0 * 4096) | 3 | WX_NX)); // хвост той же 2-МБ страницы за образом: RW+NX
    uint64_t* pd4 = ptr(ptr(pml4[0] & ADDR)[4] & ADDR);
    CHECK((pd4[0] & WX_NX) && (pd4[0] & WX_PS));                      // память выше 4 ГБ не тронута

    // Повторное применение ничего не меняет и не расходует память.
    budget = 0;
    CHECK(wx_enforce(&env, pml4, &L) == 0 && wx_scan(&env, pml4, &L, &st) == 0);

    // Нарушения находятся.
    uint64_t save = pt_a[0x1c9];
    pt_a[0x1c9] &= ~WX_NX;
    CHECK(wx_scan(&env, pml4, &L, &st) == -1 && st.bad_count == 1 && st.bad_va == 0x1c9000);
    pt_a[0x1c9] = save;
    save = pt_a[0x11d];
    pt_a[0x11d] |= WX_W;                                              // текст записываем: это W+X
    CHECK(wx_scan(&env, pml4, &L, &st) == -1 && st.bad_va == 0x11d000 && !st.layout_ok && st.layout_bad_va == 0x11d000);
    pt_a[0x11d] = save;
    save = pt_a[0x105];
    pt_a[0x105] |= WX_NX | WX_W;                                      // rodata записываем без W+X: раскладка нарушена, W+X нет
    CHECK(wx_scan(&env, pml4, &L, &st) == -1 && st.bad_count == 0 && !st.layout_ok && st.layout_bad_va == 0x105000);
    pt_a[0x105] = save;
    save = pt_a[0x120];
    pt_a[0x120] = 0;                                                  // дыра в тексте
    CHECK(wx_scan(&env, pml4, &L, &st) == -1 && st.layout_bad_va == 0x120000);
    pt_a[0x120] = save;
    CHECK(wx_scan(&env, pml4, &L, &st) == 0);

    // Эффективные права: NX на верхнем уровне перекрывает W+X листа, отсутствие W на верхнем - тоже.
    uint64_t* mpd = ptr(ptr(pml4[510] & ADDR)[0] & ADDR);
    mpd[1] = 0x20000000ULL | 0x83;                                    // 2-МБ лист W+X в верхней половине
    CHECK(wx_scan(&env, pml4, &L, &st) == -1 && st.bad_count == 1 && st.bad_va == (0xFFFFFF0000000000ULL + 2 * MB));
    ptr(pml4[510] & ADDR)[0] |= WX_NX;
    CHECK(wx_scan(&env, pml4, &L, &st) == 0);
    ptr(pml4[510] & ADDR)[0] &= ~(WX_NX | WX_W);
    CHECK(wx_scan(&env, pml4, &L, &st) == 0);
    ptr(pml4[510] & ADDR)[0] |= WX_W;
    mpd[1] = 0;

    // Отказы enforce: нет памяти под таблицу, 1-ГБ лист под образом, PAT у 2-МБ страницы под образом.
    build(); budget = 1;
    CHECK(wx_enforce(&env, pml4, &L) == -1);
    build(); budget = 10;
    ptr(pml4[0] & ADDR)[0] = 0 | 0x83;
    CHECK(wx_enforce(&env, pml4, &L) == -1);
    build(); budget = 10;
    pd_first()[1] |= (1ULL << 12);
    CHECK(wx_enforce(&env, pml4, &L) == -1);
    build(); budget = 10;
    pml4[0] = 0;
    CHECK(wx_enforce(&env, pml4, &L) == -1);

    // Без трамплина (exec_lo == exec_hi), как на ARM-подобной раскладке: страница 0x8000 остаётся RW+NX.
    wx_layout_t L2 = L; L2.exec_lo = L2.exec_hi = 0;
    build(); budget = 10;
    CHECK(wx_enforce(&env, pml4, &L2) == 0 && wx_scan(&env, pml4, &L2, &st) == 0);
    CHECK(pt_of(0)[8] == (0x8000 | 3 | WX_NX));

    printf(fails ? "wx: FAIL (%d)\n" : "wx: PASS\n", fails);
    return fails ? 1 : 0;
}
