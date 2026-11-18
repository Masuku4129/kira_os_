#ifndef KASLR_H
#define KASLR_H
// KASLR: случайный физический (он же виртуальный - у ядра identity-карта) базовый адрес образа ядра на каждой
// загрузке, x86-64. Здесь только чистая часть, без железа и без обращений к памяти за пределами переданных
// буферов, поэтому она проверяется хост-тестом tests/kaslr_test.c: сбор свободных и занятых диапазонов из
// BootInfo, выбор сдвига, применение таблицы перемещений, слова командной строки. Работа с процессором (энтропия,
// копирование образа, переход) - arch/x86_64/kaslr_arch.c; разбор ELF и построение таблицы - tools/mkkreloc.py.
#include <stdint.h>
#include "bootinfo.h"

#define KASLR_ALIGN       (2ULL << 20)          // сдвиг кратен 2 МБ: 2-МБ области карты страниц остаются целыми
#define KASLR_FLOOR       (16ULL << 20)         // ниже 16 МБ - ISA DMA (SB16) и прочее, что считает эту память "своей"
#define KASLR_CEIL        (1ULL << 30)          // образ целиком в identity-карте первого гигабайта
#define KASLR_MAX_RANGES  192

typedef struct { uint64_t lo, hi; } kaslr_range_t;      // [lo, hi)

typedef struct {
    kaslr_range_t usable[KASLR_MAX_RANGES];     // свободная ОЗУ из карты загрузчика, по возрастанию, без пересечений
    kaslr_range_t avoid[KASLR_MAX_RANGES];      // занято загрузчиком: модули, BootInfo, экран, образ в старом месте
    unsigned n_usable, n_avoid;
    uint64_t ram_top;                           // верхняя граница свободной ОЗУ (для запаса под таблицы PMM)
} kaslr_mem_t;

typedef struct {
    const kaslr_range_t* usable; unsigned n_usable;
    const kaslr_range_t* avoid;  unsigned n_avoid;
    uint64_t size;                              // образ + запас за ним (округляется вверх до KASLR_ALIGN внутри)
    uint64_t floor, ceil;
} kaslr_env_t;

// Причины, по которым сдвига нет (для журнала); 0 - сдвиг выбран.
enum {
    KASLR_OK = 0, KASLR_OFF_CMDLINE, KASLR_NO_TABLE, KASLR_BAD_BOOTINFO, KASLR_NO_SLOT, KASLR_BAD_TABLE, KASLR_NO_ENV
};
const char* kaslr_reason(int r);

// Слово командной строки (разделители - пробел и табуляция), например "kira.nokaslr". 1 - есть.
int kaslr_cmdline_has(const char* cmdline, uint32_t len, const char* word);

// Свободные и занятые диапазоны из BootInfo. Занятыми считаются модули, сам BootInfo, образ ядра там, где он лежит
// сейчас (после выбора это место освобождается, но выбирать на его месте нельзя: копирование идёт из него), экран.
// extra_lo/extra_hi - ещё один занятый диапазон (структура Multiboot2); равные - нет.
// 0 - успех, -1 - BootInfo неверен, -2 - занятых диапазонов больше KASLR_MAX_RANGES (выбирать нельзя: сдвиг мог бы
// попасть на занятое).
int kaslr_collect(const void* blob, uint32_t size, uint64_t extra_lo, uint64_t extra_hi, kaslr_mem_t* out);

// Сколько позиций (баз, кратных KASLR_ALIGN) подходит: [base, base+size) целиком в свободной памяти, в [floor, ceil),
// и не задевает занятое.
uint64_t kaslr_slots(const kaslr_env_t* e);
// Выбирает позицию по энтропии (равномерно по подходящим). 0 - база записана в *base, *slots - сколько было позиций;
// -1 - подходящих нет.
int kaslr_pick(const kaslr_env_t* e, uint64_t entropy, uint64_t* base, uint64_t* slots);

// Смешивание отсчётов энтропии (необратимое, splitmix64).
uint64_t kaslr_mix(uint64_t acc, uint64_t sample);

// Таблица перемещений образа (строит tools/mkkreloc.py, лежит в секции .kreloc образа):
//   uint32 magic 'KRLC', uint32 version, uint32 n64, uint32 n32, uint32 loaded_size, затем n64 смещений 64-битных
//   слов, затем n32 смещений 32-битных слов (R_X86_64_32/32S). Смещения - от начала образа.
#define KRELOC_MAGIC   0x434C524Bu
#define KRELOC_VERSION 1u
typedef struct { uint32_t magic, version, n64, n32, loaded_size; } kreloc_hdr_t;
// Проверка структуры таблицы: 0 - годна, -1 - нет. avail - сколько байт таблицы доступно.
int kreloc_check(const void* tbl, uint32_t avail, uint32_t image_loaded_size);
// Прибавляет slide (положительный) ко всем перечисленным словам копии образа dst (loaded_size байт). 32-битные слова
// не должны выйти за 2 ГБ (знаковое расширение R_X86_64_32S). Возвращает число исправленных слов или -1 при ошибке.
int64_t kreloc_apply(uint8_t* dst, uint32_t loaded_size, const void* tbl, uint32_t avail, uint64_t slide);

#endif
