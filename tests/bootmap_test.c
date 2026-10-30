// Хост-тест раннего окна bootmap: содержимое таблиц страниц проверяется напрямую
// (CR3, NXE и сброс TLB подменены переменными).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "bootmap.h"

uint64_t bootmap_test_cr3;
int bootmap_test_nxe;
unsigned bootmap_test_flushes;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define P_PRESENT 1ULL
#define P_WRITE 2ULL
#define P_PCD 0x10ULL
#define P_HUGE 0x80ULL
#define P_NX (1ULL << 63)

int main(void) {
    uint64_t* pml4 = aligned_alloc(4096, 4096);
    memset(pml4, 0, 4096);
    bootmap_test_cr3 = (uint64_t)(uintptr_t)pml4;

    // До инициализации ничего не отображается.
    CHECK(bootmap_map(0xFD000000ULL, 4096, 1) == 0);

    // Запись PML4[511] занята - окно не ставится и чужая запись не затирается.
    pml4[511] = 0x1234000ULL | P_PRESENT;
    CHECK(bootmap_init() == -1);
    CHECK(pml4[511] == (0x1234000ULL | P_PRESENT));
    CHECK(bootmap_map(0xFD000000ULL, 4096, 1) == 0);
    pml4[511] = 0;

    CHECK(bootmap_init() == 0);
    CHECK((pml4[511] & P_PRESENT) && (pml4[511] & P_WRITE));
    CHECK(!(pml4[511] & 4));                                  // бита USER нет: Ring 3 окно не видит
    CHECK(bootmap_init() == 0);                               // повторный вызов безопасен
    uint64_t* pdpt = (uint64_t*)(uintptr_t)(pml4[511] & ~0xFFFULL);
    CHECK((pdpt[0] & P_PRESENT) && !(pdpt[0] & 4));
    uint64_t* pd = (uint64_t*)(uintptr_t)(pdpt[0] & ~0xFFFULL);
    CHECK(bootmap_used_slots() == 0);

    // Видеопамять: 4 МБ с неровным началом -> три страницы по 2 МБ, UC, без NX (NXE ещё выключен).
    bootmap_test_nxe = 0;
    uint64_t fb = 0xFD000000ULL + 0x1234;
    uint8_t* va = bootmap_map(fb, 4 << 20, 1);
    CHECK((uint64_t)(uintptr_t)va == BOOTMAP_BASE + 0x1234);
    CHECK(((pd[0] & ~0xFFFULL) & ~(P_NX)) == 0xFD000000ULL);
    CHECK((pd[0] & (P_PRESENT | P_WRITE | P_HUGE | P_PCD)) == (P_PRESENT | P_WRITE | P_HUGE | P_PCD));
    CHECK(!(pd[0] & P_NX));
    CHECK(!(pd[0] & 4));
    CHECK((pd[1] & ~0xFFFULL) == 0xFD200000ULL);
    unsigned used_after_fb = bootmap_used_slots();
    CHECK(used_after_fb == 3);                                // [0xFD001234, +4 МБ) задевает три двухмегабайтных куска

    // Тот же диапазон - тот же адрес, новых слотов нет.
    CHECK((uint8_t*)bootmap_map(fb, 4 << 20, 1) == va);
    CHECK(bootmap_used_slots() == used_after_fb);

    // ОЗУ (WB) в другом месте, NXE включён -> NX выставлен, PCD нет.
    bootmap_test_nxe = 1;
    uint8_t* ram = bootmap_map(0x180000000ULL + 0x40, 0x100, 0);
    CHECK((uint64_t)(uintptr_t)ram == BOOTMAP_BASE + (uint64_t)used_after_fb * BOOTMAP_SLOT + 0x40);
    CHECK((pd[used_after_fb] & P_NX) && !(pd[used_after_fb] & P_PCD));
    CHECK((pd[used_after_fb] & 0x000FFFFFFFFFF000ULL) == 0x180000000ULL);
    // тот же физический кусок, но другой тип кэша - отдельный слот (смешивать типы памяти нельзя)
    uint8_t* ram_uc = bootmap_map(0x180000000ULL, 0x100, 1);
    CHECK(ram_uc && (uint8_t*)ram_uc != ram - 0x40);

    // Граничные случаи.
    CHECK(bootmap_map(0xFD000000ULL, 0, 1) == 0);
    CHECK(bootmap_map(~0ULL - 10, 100, 1) == 0);                          // переполнение
    CHECK(bootmap_map(1ULL << 52, 4096, 1) == 0);                         // за пределом физических адресов
    CHECK(bootmap_map(0x200000000ULL, 1ULL << 31, 1) == 0);               // больше окна в 1 ГБ

    // Окно заполняется до конца и честно отказывает.
    bootmap_release_all();
    CHECK(bootmap_used_slots() == 0);
    CHECK(pd[0] == 0 && pd[1] == 0);
    CHECK(bootmap_test_flushes >= 1);
    CHECK(bootmap_map(0x400000000ULL, 1ULL << 30, 1) != 0);              // ровно 512 слотов
    CHECK(bootmap_used_slots() == BOOTMAP_SLOTS);
    CHECK(bootmap_map(0x800000000ULL, 4096, 1) == 0);                    // места нет
    CHECK(pml4[511] & P_PRESENT);                                         // запись PML4 остаётся (процессы копируют её)

    free(pml4);
    if (fails) { printf("bootmap: %d FAILED\n", fails); return 1; }
    printf("bootmap: all checks passed\n");
    return 0;
}
