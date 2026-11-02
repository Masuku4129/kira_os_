#ifndef KIRA_KDRV_FORMAT_H
#define KIRA_KDRV_FORMAT_H
/* Формат пакета драйвера Ring 3 и договор драйвера с ядром (docs/DRIVERS.md). Один заголовок для ядра, программ-драйверов
   и хост-инструмента tools/kdrvpack: менять только вместе (поле format).

   /drv/ИМЯ.kdrv = kdrv_header_t + образ ELF драйвера (elf_size байт, дальше ничего).
   /drv/ИМЯ.rec  = kdrv_rec_t: что было подтверждено пользователем при установке и хеши.
   Подписей нет (решение владельца): доверие = явное подтверждение пользователя + SHA-256 в .rec, сверяемый при КАЖДОЙ загрузке
   (хеш образа ELF и хеш всего файла .kdrv, чтобы заголовок с правами и списком устройств тоже нельзя было подменить).
   Права процесса = запрошенные в заголовке И подтверждённые в .rec. Все числа - порядок байт процессора (x86-64, ARM64: little). */
#include <stdint.h>
#include "caps.h"

#define KDRV_MAGIC      0x5652444Bu        /* "KDRV" */
#define KREC_MAGIC      0x4345524Bu        /* "KREC" */
#define KREC_CONFIRMED  0x4B4F4B4Fu        /* поле confirmed: пользователь подтвердил установку */
#define KDRV_FORMAT     1
#define KDRV_NAME_MAX   11                 /* процесс называется drv-ИМЯ (имя процесса - до 15 знаков) */
#define KDRV_MATCH_MAX  8
#define KDRV_ELF_MAX    (8u << 20)
#define KDRV_ANY        0xFFFFu            /* vendor / device: любое значение */

/* Устройство подходит, если vendor и device совпали (или KDRV_ANY) И (класс & cls_mask) == (cls & cls_mask).
   cls = класс << 16 | подкласс << 8 | prog_if. Запись без vendor, без device и без маски класса (подходит всё) недопустима. */
typedef struct { uint16_t vendor, device; uint32_t cls, cls_mask; } kdrv_match_t;

typedef struct {
    uint32_t magic;                        /* KDRV_MAGIC */
    uint16_t format;                       /* KDRV_FORMAT */
    uint16_t hdr_size;                     /* sizeof(kdrv_header_t): ELF начинается здесь */
    char     name[16];                     /* [a-z0-9_-], до KDRV_NAME_MAX знаков, с нулём */
    uint32_t version;
    uint32_t caps;                         /* запрошенные права (CAP_MMIO | CAP_IRQ | CAP_DMA | CAP_PCIDEV | CAP_NETDEV) */
    uint32_t elf_size;
    uint32_t nmatch;                       /* 1..KDRV_MATCH_MAX */
    kdrv_match_t match[KDRV_MATCH_MAX];
} kdrv_header_t;

typedef struct {
    uint32_t magic;                        /* KREC_MAGIC */
    uint16_t format;
    uint16_t rec_size;                     /* sizeof(kdrv_rec_t) */
    uint8_t  elf_sha[32];                  /* SHA-256 образа ELF */
    uint8_t  pkg_sha[32];                  /* SHA-256 всего файла .kdrv */
    uint8_t  src_sha[32];                  /* SHA-256 исходников, из которых драйвер собран на Кире (нули - исходников нет) */
    uint32_t caps_ok;                      /* права, подтверждённые пользователем */
    uint32_t confirmed;                    /* KREC_CONFIRMED */
    uint64_t date;                         /* секунды UTC от 1970 при установке (0 - часов не было) */
    char     source[96];                   /* откуда драйвер (адрес, имя архива), с нулём */
} kdrv_rec_t;

_Static_assert(sizeof(kdrv_match_t) == 12, "kdrv_match_t");
_Static_assert(sizeof(kdrv_header_t) == 136, "kdrv_header_t");
_Static_assert(sizeof(kdrv_rec_t) == 216, "kdrv_rec_t");

enum { KDRV_OK = 0, KDRV_E_SHORT = -1, KDRV_E_MAGIC = -2, KDRV_E_FORMAT = -3, KDRV_E_NAME = -4, KDRV_E_CAPS = -5,
       KDRV_E_MATCH = -6, KDRV_E_SIZE = -7, KDRV_E_UNCONFIRMED = -8, KDRV_E_HASH = -9 };

/* Чистые функции (kdrv_fmt.c; проверяются на хосте). */
int kdrv_header_check(const void* file, uint32_t len, kdrv_header_t* out);      /* заголовок корректен и файл ровно hdr_size + elf_size */
int kdrv_rec_check(const void* rec, uint32_t len, kdrv_rec_t* out);             /* запись корректна и подтверждена пользователем */
int kdrv_match_dev(const kdrv_header_t* h, uint16_t vendor, uint16_t device, uint32_t cls);   /* 1 - подходит */
const char* kdrv_strerror(int err);

/* ---- Договор драйвера с ядром (системные вызовы 48..54; номера не пересекаются с syscall.c) ---- */
enum { KDRV_SYS_INFO = 48,         /* (буфер kdrv_info_t, ёмкость) -> 0: свои устройство и права */
       KDRV_SYS_CFG_READ = 49,     /* (смещение < 256, ширина 1/2/4) -> значение; CAP_PCIDEV */
       KDRV_SYS_CFG_WRITE = 50,    /* (смещение < 256, ширина 1/2/4, значение) -> 0; CAP_PCIDEV; не BAR, не MSI/MSI-X/PCIe, команда - только MEM/IO/MASTER/INTx-off */
       KDRV_SYS_MMIO_MAP = 51,     /* (номер BAR, смещение, длина) -> адрес; CAP_MMIO; страницы устройства без кэша и без исполнения */
       KDRV_SYS_DMA_ALLOC = 52,    /* (байт, предел адреса в битах или 0, kdrv_dma_t*) -> 0; CAP_DMA; освобождается при гибели драйвера */
       KDRV_SYS_IRQ_WAIT = 53,     /* (срок в 10 мс или ~0) -> маска KDRV_EV_* или KE_TIMEOUT; CAP_IRQ */
       KDRV_SYS_NETDEV = 54,       /* (операция KDRV_NET_*, аргумент, флаги) ; CAP_NETDEV */
       KDRV_SYS_PCILIST = 55 };    /* (буфер kdrv_pcient_t[], число записей, номер первой) -> записано; ТОЛЬКО ЧТЕНИЕ, право CAP_PCILIST (любая программа, не драйвер) */
#define KDRV_EV_IRQ 1u             /* пришло прерывание устройства */
#define KDRV_EV_TX  2u             /* ядро положило кадр в кольцо передачи */
#define KDRV_MAX_MMIO  16          /* отображений BAR на драйвер */
#define KDRV_MAX_MMIO_BYTES (16u << 20)   /* на одно отображение */
#define KDRV_MAX_DMA   32          /* блоков DMA на драйвер */
#define KDRV_MAX_DMA_BYTES (4u << 20)     /* на один блок */
#define KDRV_MAX_DMA_TOTAL (32u << 20)    /* всего */

typedef struct {
    uint32_t size;                         /* sizeof(kdrv_info_t) */
    uint16_t vendor, device;
    uint32_t cls;                          /* класс << 16 | подкласс << 8 | prog_if */
    uint32_t granted;                      /* права процесса */
    uint32_t flags;                        /* KDRV_INFO_* */
    uint32_t version;
    char     name[16];
    struct { uint64_t size; uint32_t flags; uint32_t pad; } bar[6];   /* flags: KDRV_BAR_* ; size 0 - BAR нет */
} kdrv_info_t;
#define KDRV_INFO_MSI 1u                   /* у устройства есть прерывание сообщением, ядро его включило */
#define KDRV_BAR_IO 1u                     /* порты ввода-вывода: отобразить нельзя */
#define KDRV_BAR_MEM64 2u
#define KDRV_BAR_PREFETCH 4u

/* Список устройств PCI для окна «Драйверы» (KDRV_SYS_PCILIST): ровно то, что нужно показать, без BAR, адресов, конфигурации и pid. */
typedef struct {
    uint16_t vendor, device;
    uint32_t cls;                          /* класс << 16 | подкласс << 8 | prog_if */
    uint8_t  bus, dev, fn, owner;          /* адрес BDF; owner - KDRV_OWN_* */
    char     name[16];                     /* KDRV_OWN_RING3: имя пакета драйвера (процесс drv-ИМЯ); иначе пусто */
} kdrv_pcient_t;
enum { KDRV_OWN_NONE = 0,                  /* ни встроенного драйвера, ни работающего драйвера Ring 3 */
       KDRV_OWN_KERNEL = 1,                /* устройство такого типа обслуживает встроенный драйвер ядра (определяется по идентификатору и классу) */
       KDRV_OWN_RING3 = 2,                 /* работает драйвер Ring 3 drv-ИМЯ */
       KDRV_OWN_BRIDGE = 3 };              /* мост или шина: драйверу Ring 3 не отдаётся никогда */
#define KDRV_PCILIST_MAX 64                /* записей за один вызов */

typedef struct { uint64_t va, phys, bytes; } kdrv_dma_t;   /* блок DMA: адрес в драйвере, адрес для устройства, размер (кратен 4 КБ) */

/* Сетевая карта драйвера: KDRV_SYS_NETDEV, операции. Кольца создаёт ядро (общая память драйвера и ядра, страницы принадлежат
   ядру и возвращаются при гибели драйвера); драйвер НЕ передаёт ядру свою память. */
enum { KDRV_NET_REGISTER = 1,      /* (адрес 6 байт MAC, 0, 0) -> адрес области kdrv_netctl_t в драйвере */
       KDRV_NET_KICK = 2 };        /* (0,0,0): в кольцо приёма положены кадры - разбудить сетевой стек */
#define KDRV_NET_SLOTS 64          /* ячеек в каждом кольце (степень двойки) */
#define KDRV_NET_SLOT  2048        /* байт на ячейку: длина (4) + кадр */
#define KDRV_NET_MAXFRAME 1536
typedef struct {
    volatile uint32_t tx_head, tx_tail;    /* передача: head пишет ядро, tail - драйвер; свободные счётчики по модулю 2^32 */
    volatile uint32_t rx_head, rx_tail;    /* приём: head пишет драйвер, tail - ядро */
    volatile uint32_t link;                /* 1 - канал есть (пишет драйвер) */
    uint32_t slots, slot_size, max_frame;  /* заполняет ядро */
    uint32_t pad[24];                      /* до 128 байт */
} kdrv_netctl_t;
typedef struct { volatile uint32_t len; uint8_t data[KDRV_NET_SLOT - 4]; } kdrv_slot_t;
#define KDRV_NET_TX_OFF 4096u                                   /* от начала области: кольцо передачи, затем приёма */
#define KDRV_NET_RX_OFF (KDRV_NET_TX_OFF + KDRV_NET_SLOTS * KDRV_NET_SLOT)
#define KDRV_NET_BYTES  (KDRV_NET_RX_OFF + KDRV_NET_SLOTS * KDRV_NET_SLOT)

#endif
