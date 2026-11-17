#ifndef BBRING_H
#define BBRING_H

#include <stdint.h>
#include <stddef.h>

/* «Чёрный ящик»: кольцо событий производительности в памяти ядра. Формат записи и файла - docs/BLACKBOX.md.
   Записать событие можно откуда угодно (поток, прерывание таймера, планировщик): без замков, без выделения памяти,
   несколько обращений к атомарным счётчикам. Сброс на диск делает отдельный поток (bbsave.c), горячий путь диска не касается.
   Файл не зависит от ядра (только <stdint.h>), поэтому кольцо, разбор файла и подпись строк проверяет хост-тест. */

#define BB_REC_SIZE   48
#define BB_RING       1024            /* записей в кольце: 48 КиБ; файл - то же кольцо плюс заголовок */
#define BB_NAME       16
#define BB_FILE_MAGIC "KBB1"
#define BB_FILE_VERSION 1u

/* Виды событий */
enum {
    BB_BOOT = 1,      /* запуск системы: a = номер запуска */
    BB_FRAME,         /* кадр цикла дисплея дольше бюджета: a = мкс, b = точек, c = шаги gfx|ввод (мкс, по 16 бит), d = середина|сборка (мкс, по 16 бит), flags = пропущено похожих, name = самый долгий этап */
    BB_LOCK,          /* ожидание замка планировщика или мьютекса ядра: a = мкс, b = замок (1 таблица, 2+n очередь ядра n, 0xFFFF мьютекс), c = ядро-владелец + 1 (мьютекс: номер потока-владельца), d = ядро ждущего, name = поток */
    BB_CPUWAIT,       /* поток ядра ждал процессор: a = мс, b = приоритет, c = ядро, на котором запущен, name = поток */
    BB_GAP,           /* пауза без тиков или кадров: a = мс, b = кто молчал (BB_GAP_*), name = цикл */
    BB_WATCHDOG,      /* сторож: a = мс без кормления, b = вид (BB_WD_*), name = причина (начало) */
    BB_MIND,          /* Разум сменил рычаг или параметр: a = рычаг/код, b = новое значение, name = что именно */
    BB_NOTE,          /* пометка человека или системы (bb save, аварийная остановка): a, b - по смыслу, name = текст */
    BB_KIND_N
};
enum { BB_GAP_CORE = 1, BB_GAP_DISPLAY = 2, BB_GAP_MACHINE = 3 };
enum { BB_WD_WARN = 1, BB_WD_STALL = 2, BB_WD_PANIC = 3 };
/* Тяжесть: 0 - сведение, 1 - заметно, 2 - тяжёлая пауза (она же просит сброс на диск вне горячего пути) */
enum { BB_SEV_INFO = 0, BB_SEV_WARN = 1, BB_SEV_SEVERE = 2 };

typedef struct {
    uint64_t t_ms;                    /* мс от начала счёта времени ядра */
    uint8_t  kind, cpu, flags, sev;
    uint32_t a, b, c, d;
    char     name[BB_NAME];
    uint32_t seq;                     /* порядковый номер события с запуска (с 1) */
} bb_rec_t;
_Static_assert(sizeof(bb_rec_t) == BB_REC_SIZE, "запись чёрного ящика - 48 байт");

typedef struct {
    char     magic[4];                /* "KBB1" */
    uint32_t version;
    uint32_t rec_size;                /* 48 */
    uint32_t count;                   /* записей в файле, от старой к новой */
    uint32_t boot_id;                 /* номер запуска, который писал файл */
    uint32_t reason;                  /* BB_SAVE_*: почему сброшено на диск */
    uint64_t saved_ms;                /* время сброса */
    uint64_t total;                   /* событий с запуска всего (в кольце лишь последние) */
    uint32_t lost;                    /* событий вытеснено из кольца с запуска */
    uint32_t crc;                     /* CRC-32 всех записей */
} bb_file_hdr_t;
_Static_assert(sizeof(bb_file_hdr_t) == 48, "заголовок файла чёрного ящика - 48 байт");

enum { BB_SAVE_REQUEST = 1, BB_SAVE_SEVERE = 2, BB_SAVE_PERIODIC = 3, BB_SAVE_SHUTDOWN = 4, BB_SAVE_BOOT = 5 };
enum { BB_FILE_OK = 0, BB_FILE_SHORT = -1, BB_FILE_MAGIC_BAD = -2, BB_FILE_VERSION_BAD = -3, BB_FILE_SIZE_BAD = -4, BB_FILE_CRC_BAD = -5 };

/* Источники времени и номера ядра: ядро подключает их при запуске (до этого время 0 и ядро 0). */
void bb_set_hooks(uint64_t (*now_ms)(void), unsigned (*cpu_id)(void));

/* Запись события: без замков, из любого контекста. */
void bb_event(uint8_t kind, uint8_t sev, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint8_t flags, const char* name);

/* Свёрнутые счётчики (считают и события, не попавшие в кольцо из-за ограничения частоты: bb_note_suppressed). */
typedef struct {
    uint64_t total;                   /* записано событий с запуска (или с bb_clear) */
    uint64_t lost;                    /* вытеснено из кольца */
    uint64_t suppressed;              /* не записано из-за ограничения частоты (но учтено в наихудших значениях) */
    uint32_t kind_n[BB_KIND_N];       /* событий каждого вида */
    uint32_t worst[BB_KIND_N];        /* наибольшее значение a по виду (кадр - мкс, замок - мкс, ожидание и пауза - мс) */
    uint64_t worst_t_ms[BB_KIND_N];   /* когда оно случилось */
    uint32_t severe;                  /* число тяжёлых событий (рост - повод сбросить на диск) */
} bb_stats_t;
void bb_get_stats(bb_stats_t* out);
/* Учесть событие, которое в кольцо не пошло (ограничение частоты): счётчик вида и наихудшее значение сохраняются. */
void bb_note_suppressed(uint8_t kind, uint32_t a);
uint64_t bb_total(void);
uint32_t bb_severe_count(void);

/* Копия кольца: от старой записи к новой, не больше max. Возвращает число записей. Записи, которые пишутся в этот миг, пропускаются. */
unsigned bb_snapshot(bb_rec_t* out, unsigned max);
void bb_clear(void);

/* Ограничитель частоты: не больше per_sec событий в окне 1 с; skipped - сколько отброшено с прошлого пропущенного. */
typedef struct { uint64_t win_ms; uint32_t n, skipped; } bb_gate_t;
int bb_gate_pass(bb_gate_t* g, uint64_t now_ms, uint32_t per_sec, uint32_t* skipped_before);

/* Файл: сборка и разбор. build пишет заголовок и записи (out >= sizeof(bb_file_hdr_t) + n * 48); возвращает длину. */
unsigned bb_file_build(uint8_t* out, const bb_rec_t* recs, unsigned n, uint32_t boot_id, uint32_t reason,
                       uint64_t saved_ms, uint64_t total, uint32_t lost);
/* seal: записи уже лежат в buf сразу за заголовком (buf + 48); записывает заголовок и считает CRC. Возвращает длину файла. */
unsigned bb_file_seal(uint8_t* buf, unsigned n, uint32_t boot_id, uint32_t reason, uint64_t saved_ms, uint64_t total, uint32_t lost);
/* parse: проверяет заголовок, размеры и CRC; записи копирует в recs (не больше max). Возвращает число записей или код BB_FILE_*. */
int bb_file_parse(const uint8_t* buf, unsigned len, bb_file_hdr_t* hdr, bb_rec_t* recs, unsigned max);
uint32_t bb_crc32(const void* data, size_t len);

/* Человекочитаемая строка записи (без перевода строки). Возвращает длину. */
unsigned bb_format(const bb_rec_t* r, char* out, unsigned cap);
/* Сводка одной строкой для fb. */
unsigned bb_summary(char* out, unsigned cap);
/* Последние n записей через вывод out (для аварийного отчёта: паника, сторож). */
void bb_dump_tail(unsigned n, void (*out)(const char*));

const char* bb_kind_name(unsigned kind);

#endif
