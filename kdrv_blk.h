#ifndef KIRA_KDRV_BLK_H
#define KIRA_KDRV_BLK_H
#include <stdint.h>

/* Блочное устройство драйвера Ring 3 (право CAP_BLKDEV, системный вызов KDRV_SYS_BLKDEV): договор драйвера с ядром и чистые проверки.
   Образец - сетевая карта драйвера (CAP_NETDEV): кольца создаёт ЯДРО (общая память, страницы принадлежат ядру и возвращаются при гибели
   драйвера), драйвер НЕ передаёт ядру свою память и указатели. Ядро видит диск как обычный blk_dev_t (blk.c): файловые системы и хранилище
   опыта не знают, на чём лежит диск. Всё, что ядро читает из общей памяти, считается недоверенным: читается один раз, проверяется, только
   потом используется (kdrv_blk_ring_avail, kdrv_blk_track_complete).
   Общая память: страница управления (kdrv_blkctl_t с кольцами запросов и ответов), затем KDRV_BLK_SLOTS буферов данных по KDRV_BLK_SLOT_BYTES.
   Запрос ядра - ячейка кольца req (операция, сектор, число секторов, номер буфера); ответ драйвера - ячейка кольца rsp (номер запроса,
   статус). Данные записи ядро кладёт в буфер ДО запроса, данные чтения драйвер кладёт в буфер ДО ответа. Устройству (DMA) отдаётся только
   область буферов данных, и только если у драйвера есть CAP_DMA (адрес в data_phys; без права - 0); страница управления для устройства закрыта. */

#define KDRV_BLK_SECTOR      512u
#define KDRV_BLK_SLOTS       8u                                  /* запросов в полёте (степень двойки): и ячеек в каждом кольце, и буферов данных */
#define KDRV_BLK_SLOT_SECTORS 128u                               /* секторов в буфере данных: 64 КБ; большие операции ядро режет на такие куски */
#define KDRV_BLK_SLOT_BYTES  (KDRV_BLK_SLOT_SECTORS * KDRV_BLK_SECTOR)
#define KDRV_BLK_DATA_OFF    4096u                               /* от начала области: буферы данных */
#define KDRV_BLK_BYTES       (KDRV_BLK_DATA_OFF + KDRV_BLK_SLOTS * KDRV_BLK_SLOT_BYTES)
#define KDRV_BLK_MAX_SECTORS (1ull << 48)                        /* предел ёмкости, при котором сложение сектор+число не переполняется */
#define KDRV_BLK_NAME_MAX    11                                  /* знаков имени (blk_info_t.id - 12 с нулём) */

enum { KDRV_BLK_REGISTER = 1,      /* (kdrv_blkreg_t*, 0) -> адрес области kdrv_blkctl_t в драйвере; ядро заводит диск в blk.c после возврата */
       KDRV_BLK_KICK = 2 };        /* (0, 0): драйвер положил ответы в кольцо rsp - разбудить ядро */
enum { KDRV_BLK_OP_READ = 1, KDRV_BLK_OP_WRITE = 2, KDRV_BLK_OP_FLUSH = 3 };
#define KDRV_BLK_F_RO    1u        /* диск только для чтения: ядро само отказывает записи, до драйвера она не доходит */
#define KDRV_BLK_F_FLUSH 2u        /* у устройства есть кэш записи: операция FLUSH имеет смысл (иначе ядро отвечает успехом само) */
#define KDRV_BLK_F_KNOWN (KDRV_BLK_F_RO | KDRV_BLK_F_FLUSH)

typedef struct {
    char     name[12];             /* короткое имя диска: [a-z0-9], 1..11 знаков, с нулём (попадёт в blk_info_t.id: vdb) */
    uint64_t sectors;              /* ёмкость в секторах по 512 байт: 16 .. 2^48 */
    uint32_t sector_size;          /* размер сектора устройства: сейчас ровно 512 (ядро и файловые системы работают секторами по 512 байт) */
    uint32_t flags;                /* KDRV_BLK_F_* */
    uint32_t dma_bits;             /* сколько битов адреса достаёт устройство (20..63), 0 - без предела: буферы данных лягут ниже этого предела */
    uint32_t pad;
} kdrv_blkreg_t;

typedef struct { uint32_t id; uint32_t op; uint64_t lba; uint32_t count; uint32_t slot; uint64_t pad; } kdrv_blkreq_t;   /* 32 байта */
typedef struct { uint32_t id; int32_t status; } kdrv_blkrsp_t;                                                          /* 0 - выполнено, иначе ошибка */

typedef struct {
    volatile uint32_t req_head, req_tail;    /* запросы: head пишет ядро, tail - драйвер; счётчики по модулю 2^32 */
    volatile uint32_t rsp_head, rsp_tail;    /* ответы: head пишет драйвер, tail - ядро */
    uint32_t slots, slot_sectors, flags, sector_size;   /* заполняет ядро (драйвер их не меняет, ядро их назад не читает) */
    uint64_t sectors;                        /* ёмкость, как зарегистрировано */
    uint64_t data_phys;                      /* адрес буферов данных для устройства (буфер i - data_phys + i * KDRV_BLK_SLOT_BYTES); 0 - у драйвера нет CAP_DMA */
    uint32_t pad[8];
    kdrv_blkreq_t req[KDRV_BLK_SLOTS];
    kdrv_blkrsp_t rsp[KDRV_BLK_SLOTS];
} kdrv_blkctl_t;
_Static_assert(sizeof(kdrv_blkreq_t) == 32, "kdrv_blkreq_t");
_Static_assert(sizeof(kdrv_blkctl_t) <= KDRV_BLK_DATA_OFF, "kdrv_blkctl_t must fit before data");
_Static_assert((KDRV_BLK_SLOTS & (KDRV_BLK_SLOTS - 1)) == 0, "slots power of two");

/* Коды чистых проверок. */
enum { KDRV_BLK_OK = 0, KDRV_BLK_E_NAME = -1, KDRV_BLK_E_SIZE = -2, KDRV_BLK_E_SECTORS = -3, KDRV_BLK_E_FLAGS = -4, KDRV_BLK_E_BITS = -5,
       KDRV_BLK_E_OP = -6, KDRV_BLK_E_RANGE = -7, KDRV_BLK_E_SLOT = -8, KDRV_BLK_E_RO = -9 };

int kdrv_blk_reg_check(const kdrv_blkreg_t* r);                 /* регистрация: имя, сектор 512, ёмкость, флаги, dma_bits */
/* Запрос к устройству: операция, границы (сектор + число <= ёмкость без переполнения), число <= буфера, номер буфера, запись на RO-диск. */
int kdrv_blk_req_check(const kdrv_blkreq_t* q, uint64_t sectors, uint32_t flags);
/* Сколько записей в кольце готово: head - tail; -1, если больше ячеек кольца (head вне кольца - писал не тот, кому доверяем). */
int kdrv_blk_ring_avail(uint32_t head, uint32_t tail, uint32_t slots);

/* Учёт запросов в полёте на стороне ядра: номер запроса -> буфер данных. Ответ с неизвестным или уже закрытым номером отвергается. */
enum { KDRV_BLK_T_FREE = 0, KDRV_BLK_T_POSTED = 1 };
typedef struct { uint32_t id[KDRV_BLK_SLOTS]; uint8_t state[KDRV_BLK_SLOTS]; uint32_t next_id; uint32_t inflight; } kdrv_blk_track_t;
void kdrv_blk_track_init(kdrv_blk_track_t* t);
int  kdrv_blk_track_alloc(kdrv_blk_track_t* t, uint32_t* id);   /* номер буфера или -1 (все заняты); в *id - новый номер запроса */
int  kdrv_blk_track_complete(kdrv_blk_track_t* t, uint32_t id); /* номер буфера или -1 (такого запроса нет в полёте); буфер освобождается */

#endif
