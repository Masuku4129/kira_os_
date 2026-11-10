#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define PMM_PAGE_SIZE 4096UL

// Совместимый вход Multiboot2; структура сначала нормализуется в BootInfo v1.
// Возвращает 0 при успехе, отрицательное значение при ошибке.
int pmm_init(unsigned long magic, unsigned long addr);
// Основной вход: PMM работает только с проверенной картой памяти BootInfo v1.
int pmm_init_bootinfo(const void* blob, uint32_t blob_size,
                      uint64_t reserve_addr, uint64_t reserve_size);

// Возвращает физический адрес свободной страницы или 0, если памяти нет.
// (Страница 0 всегда зарезервирована, поэтому 0 - однозначный признак ошибки.)
uint64_t pmm_alloc_page(void);

// Освобождает страницу. 0 - успех; -1 - адрес вне диапазона/не выровнен,
// зарезервирован, либо страница уже свободна (double free).
int pmm_free_page(uint64_t paddr);

// Блок из pages подряд идущих страниц (0 - нет такого блока). Освобождать
// pmm_free_contiguous с теми же аргументами; 0 - успех, -1 - хотя бы одна страница отказала.
uint64_t pmm_alloc_contiguous(uint32_t pages);
uint64_t pmm_alloc_contiguous_below(uint32_t pages, uint64_t limit_exclusive);
/* Exact-size run whose physical start is aligned to align_pages (power of two), below an exclusive address limit. */
uint64_t pmm_alloc_contiguous_aligned_below(uint32_t pages, uint32_t align_pages, uint64_t limit_exclusive);
int pmm_free_contiguous(uint64_t paddr, uint32_t pages);

uint32_t pmm_get_free_memory_kb(void);

// Память выше 1 ГБ. Менеджер знает о ней с самого начала, но выдавать её нельзя, пока она не отображена
// (ramext.c): до тех пор страницы заняты и зарезервированы. pmm_high_ranges отдаёт упорядоченные,
// слитые диапазоны доступной ОЗУ выше 1 ГБ; pmm_add_high включает отображённую часть в выдачу
// и возвращает число добавленных страниц (-1: диапазон не лежит целиком внутри такого диапазона).
typedef struct { uint64_t start, end; } pmm_range_t;
#define PMM_MAX_HIGH 256
unsigned pmm_high_ranges(pmm_range_t* out, unsigned max);
int64_t  pmm_add_high(uint64_t start, uint64_t end);
unsigned pmm_acpi_ranges(pmm_range_t* out, unsigned max);   // ACPI reclaimable/NVS из карты памяти (читать можно, выдавать нельзя)
uint64_t pmm_managed_top(void);       // верхняя граница управляемых адресов (после округления)
uint32_t pmm_get_total_memory_kb(void);

#endif
