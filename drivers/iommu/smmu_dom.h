#ifndef KIRA_SMMU_DOM_H
#define KIRA_SMMU_DOM_H
/* ARM SMMUv3, чистая логика: записи таблицы потоков (STE), дескриптор контекста (CD), таблицы страниц первой стадии (формат VMSAv8-64,
   гранула 4 КБ, 4 уровня), команды очереди и разбор записей очереди событий. Первая стадия выбрана потому, что она есть везде (в QEMU 8.2 второй
   стадии нет); права: только чтение или чтение+запись (плюс признак окна MMIO) (запись без чтения первая стадия выразить не может). Железо и ядро - iommu_smmu.c. Проверка на хосте: tests/smmu_dom_test.c. */
#include <stdint.h>
#include <stddef.h>

enum { SMMU_D_OK = 0, SMMU_D_ARG = -1, SMMU_D_NOMEM = -2, SMMU_D_EXIST = -3, SMMU_D_NOENT = -4 };

/* STE - 8 слов по 64 бита. */
#define SMMU_STE_WORDS 8
void smmu_ste_abort(uint64_t ste[SMMU_STE_WORDS]);                       /* DMA отвергается */
void smmu_ste_bypass(uint64_t ste[SMMU_STE_WORDS]);                      /* без трансляции */
/* Трансляция первой стадии: запись указывает на один дескриптор контекста (физический адрес, выровнен на 64 байта). */
int  smmu_ste_s1(uint64_t ste[SMMU_STE_WORDS], uint64_t cd_phys);
#define SMMU_CD_WORDS 8
/* Дескриптор контекста: корень таблицы (выровнен на 4 КБ), asid, oas - код размера выходного адреса (IDR5.OAS: 0=32 ... 5=48). */
int  smmu_cd_s1(uint64_t cd[SMMU_CD_WORDS], uint64_t root_phys, uint16_t asid, unsigned oas);
/* Порядок записи: слова 1..7, барьер, слово 0 (V и Config) - так устройство не увидит наполовину записанную запись. */

typedef struct {
    void* ctx;
    void* (*alloc)(void* ctx, uint64_t* phys);          /* нулевая страница 4 КБ */
    void  (*release)(void* ctx, void* virt, uint64_t phys);
    void* (*virt)(void* ctx, uint64_t phys);
} smmu_mem_t;
#define SMMU_MAP_R 1u
#define SMMU_MAP_W 2u
#define SMMU_MAP_DEV 4u      /* страница - окно MMIO (память устройства nGnRE, не кэшируется): дверной звонок MSI */
#define SMMU_IPA_BITS 48
int  smmu_pt_map(uint64_t* root, uint64_t ipa, uint64_t phys, unsigned rw, unsigned oas, const smmu_mem_t* m);
int  smmu_pt_unmap(uint64_t* root, uint64_t ipa, const smmu_mem_t* m);
int  smmu_pt_lookup(uint64_t* root, uint64_t ipa, uint64_t* phys, unsigned* rw, const smmu_mem_t* m);
void smmu_pt_free(uint64_t* root, uint64_t root_phys, const smmu_mem_t* m);

/* Команды: две 64-битные записи. */
void smmu_cmd_cfgi_ste(uint64_t c[2], uint32_t sid);
void smmu_cmd_cfgi_cd_all(uint64_t c[2], uint32_t sid);                   /* сбросить кэш дескрипторов контекста потока */
void smmu_cmd_tlbi_nh_all(uint64_t c[2]);                                 /* TLBI_NH_ALL: вся трансляция первой стадии */
void smmu_cmd_sync(uint64_t c[2]);

/* Запись очереди событий: 4 слова по 64 бита. */
typedef struct { uint8_t id; uint32_t sid; uint64_t addr; int is_write; } smmu_event_t;
void smmu_event_decode(const uint64_t e[4], smmu_event_t* out);
const char* smmu_event_name(uint8_t id);
#endif
