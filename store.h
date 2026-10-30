#ifndef STORE_H
#define STORE_H
#include <stdint.h>
#include <stddef.h>
#include "blk.h"

/* Постоянное хранилище «опыта» организма. Две ячейки (LBA 0 и 1) с поколениями и CRC32:
   запись идёт в ту ячейку, где лежит СТАРОЕ поколение, поэтому обрыв питания посреди записи
   портит максимум одну ячейку, а прошлое состояние остаётся целым. */

#define EXP_QMAX 5

typedef struct {
    uint8_t  digest[32];
    uint32_t size;
    uint32_t verdict;
    char     reason[32];
} exp_quarantine_t;

typedef struct {
    uint32_t boot_count;
    uint32_t last_session_s;
    uint64_t total_uptime_s;
    uint32_t load_ema_x100;     /* выученная «норма» нагрузки CPU, % * 100 */
    uint32_t mem_ema_x100;
    uint32_t peak_stress;       /* с v23: последнее здоровье по оценке Разума */
    uint32_t curiosity;         /* не используется (поле прежнего формата сохранено, чтобы опыт не сбрасывался) */
    uint32_t anomalies;         /* сколько раз нагрузка резко отклонялась от нормы */
    uint32_t qcount;
    exp_quarantine_t q[EXP_QMAX];
} experience_t;

typedef struct {
    blk_dev_t* dev;
    uint64_t generation;
} store_t;

uint32_t crc32(const void* data, size_t len);

/* 1 - состояние восстановлено, 0 - валидных записей нет (в *out нули), <0 - ошибка устройства. */
int store_mount(store_t* s, blk_dev_t* dev, experience_t* out);
/* 1 - в секторе 0 или 1 есть корректная запись опыта (диск принадлежит Кире). Только чтение. */
int store_detect(blk_dev_t* dev);
/* 0 - записано и сброшено на диск. */
int store_commit(store_t* s, const experience_t* e);

#endif
