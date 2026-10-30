// Хост-тест политики кучи программ: пределы зависят от объёма ОЗУ, границы, запас ядру, учёт таблиц страниц.
#include <stdio.h>
#include <stdint.h>
#include "uheap.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

#define MB 1024ULL
#define GB (1024ULL * 1024)

int main(void) {
    CHECK(UHEAP_VA % 4096 == 0);
    CHECK(UHEAP_VA >= 0x8001000000ULL + 800ULL * 600 * 4);                    /* выше окна графических поверхностей */
    CHECK(UHEAP_VA_END == 0x10000000000ULL);                                  /* конец первой пользовательской записи PML4 */

    /* Предел растёт с ОЗУ: половина памяти, но не меньше 1 МБ и не больше окна адресов. */
    CHECK(uheap_quota_pages(0) == UHEAP_MIN_QUOTA);
    CHECK(uheap_quota_pages(128 * MB) == 16384);                              /* 64 МБ из 128 МБ */
    CHECK(uheap_quota_pages(1 * GB) == 131072);                               /* 512 МБ из 1 ГБ */
    CHECK(uheap_quota_pages(64 * GB) == 8388608);                             /* 32 ГБ из 64 ГБ */
    CHECK(uheap_quota_pages(~0ULL / 2) == (UHEAP_VA_END - UHEAP_VA) / 4096);  /* упирается в окно, не выходит за него */
    for (uint64_t kb = 0; kb < 64 * GB; kb += 37 * MB + 11)
        CHECK(uheap_quota_pages(kb + 1) >= uheap_quota_pages(kb));            /* монотонность */
    CHECK(UHEAP_VA + uheap_quota_pages(~0ULL / 2) * 4096 <= UHEAP_VA_END);

    /* Запас ядру: 1/16 ОЗУ, не меньше 4 МБ. */
    CHECK(uheap_reserve_kb(16 * MB) == UHEAP_MIN_RESERVE_KB);
    CHECK(uheap_reserve_kb(1 * GB) == 64 * MB);
    CHECK(uheap_reserve_kb(64 * GB) == 4 * GB);

    const uint64_t T = 1 * GB, PLENTY = T;                                    /* машина с 1 ГБ, всё свободно */
    uint64_t q = uheap_quota_pages(T);
    CHECK(uheap_check(0, 1, PLENTY, T) == UHEAP_OK);
    CHECK(uheap_check(0, 0, PLENTY, T) == UHEAP_E_ARG);                       /* ноль страниц */
    CHECK(uheap_check(0, q, PLENTY, T) == UHEAP_OK);                          /* ровно предел */
    CHECK(uheap_check(0, q + 1, PLENTY, T) == UHEAP_E_ARG);
    CHECK(uheap_check(q - 1000, 1000, PLENTY, T) == UHEAP_OK);
    CHECK(uheap_check(q - 1000, 1001, PLENTY, T) == UHEAP_E_QUOTA);           /* суммарно больше предела */
    CHECK(uheap_check(q, 1, PLENTY, T) == UHEAP_E_QUOTA);
    CHECK(uheap_check(~0ULL, 1, PLENTY, T) == UHEAP_E_QUOTA);                 /* испорченный счётчик не даёт обойти предел */
    CHECK(uheap_check(0, ~0ULL, PLENTY, T) == UHEAP_E_ARG);
    /* Малая машина: тот же запрос, который проходит на 1 ГБ, на 128 МБ превышает предел. */
    CHECK(uheap_check(0, 100000, PLENTY, 128 * MB) == UHEAP_E_ARG);
    CHECK(uheap_check(0, 100000, PLENTY, T) == UHEAP_OK);

    /* Память: запас остаётся ядру, таблицы страниц учтены. */
    uint64_t need = (uint64_t)(100 + 100 / 512 + 3) * 4 + uheap_reserve_kb(T);
    CHECK(uheap_check(0, 100, need, T) == UHEAP_OK);
    CHECK(uheap_check(0, 100, need - 1, T) == UHEAP_E_MEM);
    CHECK(uheap_check(0, 1, 0, T) == UHEAP_E_MEM);
    /* Крупная машина: запас выше, чем на маленькой, значит свободных 100 МБ из 64 ГБ программе не отдаются. */
    CHECK(uheap_check(0, 1000, 100 * MB, 64 * GB) == UHEAP_E_MEM);
    CHECK(uheap_check(0, 1000, 5 * GB, 64 * GB) == UHEAP_OK);

    if (fails) { printf("uheap: %d FAILED\n", fails); return 1; }
    printf("uheap: machine-dependent quota and reserve, argument, quota, memory and overflow checks passed\n");
    return 0;
}
