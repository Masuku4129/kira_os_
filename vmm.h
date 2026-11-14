#ifndef VMM_H
#define VMM_H

#include <stdint.h>

// Флаги отображения (Present выставляется автоматически)
#define VMM_WRITE   (1ULL << 1)
#define VMM_USER    (1ULL << 2)
#define VMM_NOCACHE (1ULL << 4)
#define VMM_WC      (1ULL << 3)   // write-combining; только после vmm_enable_write_combining()
#define VMM_NOEXEC  (1ULL << 63)
#define VMM_SHARED  (1ULL << 9)    // страница принадлежит не процессу (её не освобождает vmm_space_destroy)

// Проверяет, что процессор действительно в long mode с включённым paging,
// включает EFER.NXE и CR0.WP, выполняет самотест map/translate/unmap.
// Возвращает 0 при успехе.
int vmm_init(void);

// Отображает одну 4-КБ страницу. 0 - успех, -1 - ошибка (не выровнено,
// неканонический адрес, уже отображено, адрес попадает в 2-МБ страницу,
// нет памяти под таблицы).
// Программирует IA32_PAT: запись 1 = write-combining (бит PWT в PTE выбирает её).
// Нужно для видеопамяти: без WC каждая запись пикселя - отдельная транзакция шины.
// 0 - успех, -1 - процессор не поддерживает PAT.
int vmm_enable_write_combining(void);

int vmm_map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);

// Регистры устройства или видеопамять: отображение в окне MMIO ядра (общем для всех адресных пространств).
// cache: VMM_MMIO_UC - без кэша (регистры), VMM_MMIO_WC - write-combining (видеопамять; если PAT нет - без кэша).
// Повторный запрос внутри уже отображённой области возвращает тот же адрес. 0 - не удалось.
#define VMM_MMIO_UC 0
#define VMM_MMIO_WC 1
volatile void* vmm_map_mmio(uint64_t phys, uint64_t len, int cache);

// Снимает отображение 4-КБ страницы. 0 - успех, -1 - страница не отображена.
int vmm_unmap_page(uint64_t virtual_addr);

// Трансляция виртуального адреса в физический (учитывает 4-КБ и 2-МБ страницы).
// 0 - успех, -1 - адрес не отображён.
// Разбивает первую 2-МБ страницу на 4-КБ и снимает отображение страницы 0
// (ловушка на NULL). 0 - успех.
int vmm_guard_null_page(void);

int vmm_translate(uint64_t virtual_addr, uint64_t* physical_addr);

// --- Адресные пространства процессов ---
// Раскладка памяти программы одна для всех архитектур, сдвинута на VMM_USER_SHIFT своей архитектуры (arch_vmm.h):
// x86-64 - пользовательская половина PML4[1..255] (нижние 512 ГБ - только ядро); ARM64 - верхняя половина TTBR0
// (нижняя занята тождественной картой ядра).
#include "arch_vmm.h"
#define VMM_USER_BASE USER_VA(0x0000008000000000ULL)
#define VMM_USER_TOP  USER_VA(0x0000800000000000ULL)

uint64_t vmm_kernel_root(void);
void     vmm_switch(uint64_t root);
uint64_t vmm_space_create(void);                       // 0 - нет памяти
int      vmm_space_map(uint64_t root, uint64_t va, uint64_t pa, uint64_t flags);
unsigned vmm_space_destroy(uint64_t root);             // число освобождённых страниц данных
// Снять отображение страницы в пространстве root (страница не освобождается). TLB не трогает: после снятия
// вызывающий обязан сделать vmm_tlb_shootdown - пространство может быть загружено на других ядрах.
int      vmm_space_unmap(uint64_t root, uint64_t va);
// Сбросить записи TLB [va, va + pages*4096) пространства root на всех ядрах, где оно загружено (межъядерное
// прерывание и ожидание ответа). Вызывать с включёнными прерываниями и без спин-замков (реализация - smp.c).
/* Ожидает ack всех CPU с этим root; после 200 мс только предупреждает и продолжает ждать. Возврат означает безопасный reclaim. */
void     vmm_tlb_shootdown(uint64_t root, uint64_t va, uint32_t pages);
int      vmm_user_range_ok(uint64_t va, uint64_t len, int write);
// Стеки потоков ядра с пустотой ниже (переполнение - #PF/#DF, а не порча памяти). bytes кратно 4 КБ, одинаково для всех.
#include "kstack.h"
int      vmm_enable_smep(void);
void     vmm_cpu_init_ap(void);                        // CR0.WP, SMEP, PAT на не загрузочном процессоре                        // 1 - включён, 0 - процессор не умеет

#endif
