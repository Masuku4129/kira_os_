#ifndef KIRA_AMD_VI_H
#define KIRA_AMD_VI_H
/* AMD-Vi (AMD I/O Virtualization Technology): чистая логика без обращений к железу - разбор таблицы IVRS, запись в таблицу
   устройств (DTE) и страницы трансляции формата v1. Регистры и кольца команд - в iommu_amd.c. Всё проверяется на хосте
   (tests/amdvi_test.c), железо - в QEMU (-device amd-iommu, tests/amdvi_boot_test.sh). Только сегмент PCI 0. */
#include <stdint.h>
#include <stddef.h>

#define AMDVI_MAX_UNITS    8
#define AMDVI_MAX_RANGES   64     /* диапазоны устройств на все блоки IVHD */
#define AMDVI_MAX_IVMD     32
#define AMDVI_MAX_SPECIAL  16
#define AMDVI_PAGE         4096u

enum { AMDVI_OK = 0, AMDVI_E_SHORT = -1, AMDVI_E_FORMAT = -2, AMDVI_E_LIMIT = -3, AMDVI_E_NOUNIT = -4, AMDVI_E_ARG = -5, AMDVI_E_NOMEM = -6,
       AMDVI_E_EXIST = -7, AMDVI_E_NOENT = -8 };

typedef struct { uint16_t lo, hi; uint8_t unit; uint8_t alias; } amdvi_range_t;      /* устройства lo..hi покрывает блок unit; alias=1 - запись с псевдонимом */
typedef struct { uint16_t lo, hi; uint64_t start, len; uint8_t flags; } amdvi_ivmd_t;   /* память, нужная микропрограмме (аналог RMRR) */
typedef struct { uint16_t devid; uint8_t variety; uint8_t handle; } amdvi_special_t;   /* IOAPIC / HPET: свои DTE для прерываний */
typedef struct {
    uint64_t base;               /* MMIO-регистры */
    uint16_t devid;              /* PCI-функция самого IOMMU */
    uint8_t  type;               /* 0x10 / 0x11 / 0x40 */
    uint64_t efr;                /* регистр расширенных возможностей из IVHD (0 для типа 0x10) */
} amdvi_unit_t;
typedef struct {
    uint32_t ivinfo;
    unsigned nunits, nranges, nivmd, nspecial;
    amdvi_unit_t unit[AMDVI_MAX_UNITS];
    amdvi_range_t range[AMDVI_MAX_RANGES];
    amdvi_ivmd_t ivmd[AMDVI_MAX_IVMD];
    amdvi_special_t special[AMDVI_MAX_SPECIAL];
} amdvi_info_t;

/* Разбор IVRS (с заголовком ACPI): строгие границы, неизвестные записи пропускаются по их длине, переполнение таблиц - ошибка. */
int amdvi_ivrs_parse(const void* table, size_t len, amdvi_info_t* out);
int amdvi_unit_of(const amdvi_info_t* i, uint16_t devid);          /* номер блока, покрывающего устройство; -1 - никто; -2 - покрыто только через псевдоним (не изолируем) */
int amdvi_ivmd_hit(const amdvi_info_t* i, uint16_t devid);         /* 1 - микропрограмме нужна память для этого устройства */
int amdvi_is_special(const amdvi_info_t* i, uint16_t devid);

/* DTE - четыре слова по 64 бита. */
#define AMDVI_DTE_WORDS 4
void amdvi_dte_passthrough(uint64_t dte[AMDVI_DTE_WORDS]);                                         /* без трансляции, DMA разрешён */
void amdvi_dte_translated(uint64_t dte[AMDVI_DTE_WORDS], uint64_t root_phys, uint16_t domain);     /* 4 уровня, пустая таблица */
void amdvi_dte_blocked(uint64_t dte[AMDVI_DTE_WORDS]);                                             /* запись недействительна: DMA отвергается */

/* Страницы трансляции v1: 4 уровня, лист 4 КБ. */
typedef struct {
    void* ctx;
    void* (*alloc)(void* ctx, uint64_t* phys);          /* нулевая страница, выровнена на 4 КБ; 0 - нет памяти */
    void  (*release)(void* ctx, void* virt, uint64_t phys);
    void* (*virt)(void* ctx, uint64_t phys);
} amdvi_mem_t;
#define AMDVI_MAP_R 1u
#define AMDVI_MAP_W 2u
#define AMDVI_IOVA_BITS 48
int  amdvi_pt_map(uint64_t* root, uint64_t iova, uint64_t phys, unsigned rw, const amdvi_mem_t* m);
int  amdvi_pt_unmap(uint64_t* root, uint64_t iova, const amdvi_mem_t* m);        /* AMDVI_E_NOENT, если страница не отображена */
int  amdvi_pt_lookup(uint64_t* root, uint64_t iova, uint64_t* phys, unsigned* rw, const amdvi_mem_t* m);   /* 0 - отображена */
void amdvi_pt_free(uint64_t* root, uint64_t root_phys, const amdvi_mem_t* m);     /* освободить всё дерево, включая корень */

/* Команды кольца: 16 байт, код в старших 4 битах второго слова. */
void amdvi_cmd_completion_wait(uint32_t cmd[4], uint64_t store_phys, uint64_t data);
void amdvi_cmd_inv_devtab(uint32_t cmd[4], uint16_t devid);
void amdvi_cmd_inv_pages_all(uint32_t cmd[4], uint16_t domain);                  /* вся трансляция домена */
#endif
