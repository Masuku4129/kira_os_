#ifndef KIRA_ROLLBACK_H
#define KIRA_ROLLBACK_H
/* Защита от отката (А3): чистая политика, общая для UEFI-загрузчика, ядра и хост-тестов. Без libc, без выделения памяти, без обращения к TPM
   (его команды - tpm2.h, передача байтов - у вызывающего). Здесь только решения: что считать откатом и что делать с TPM.

   1. Откат образа. Выпуск из подписанного манифеста (bootsig.h) сравнивается с монотонным счётчиком TPM: меньше - отказ. Счётчик может расти
      только на 1 за команду (так устроен NV-счётчик), поэтому номер выпуска - небольшое порядковое число (1, 2, 3, ...), а не дата или хэш;
      за одну загрузку счётчик поднимается не больше чем на RB_MAX_RAISE (если разрыв больше - поднимается на это число, остаток - следующими
      загрузками: пол остаётся не выше выпуска, то есть защита не ломает законный образ).
   2. Откат диска. «Эпоха» тома KiraFS растёт при записях, а её последнее подтверждённое значение хранится в TPM (якорь). Том с эпохой меньше якоря -
      старая копия диска. */

#include <stdint.h>
#include <stddef.h>
#include "tpm2.h"

#define RB_MAX_RAISE 256u    /* предел увеличений счётчика за одну загрузку (каждое - запись в NV) */

enum {
    RB_OK = 0,
    RB_E_ARGS = -1,
    RB_E_ROLLBACK_TPM = -2,     /* выпуск ниже счётчика TPM */
    RB_E_ROLLBACK_MIN = -3,     /* выпуск ниже вшитого порога KIRA_MIN_IMAGE_VERSION */
    RB_E_NO_TPM = -4            /* TPM обязателен (KIRA_REQUIRE_TPM), но его нет или счётчик недоступен */
};

/* Источник решения о выпуске: честная строка в журнал обязана сказать, чем защита обеспечена. */
typedef enum {
    RB_SRC_TPM_COUNTER = 0,     /* сверено со счётчиком TPM (и с порогом, если он задан) */
    RB_SRC_STATIC_MIN  = 1      /* TPM нет или счётчик недоступен: только вшитый порог KIRA_MIN_IMAGE_VERSION */
} rb_source_t;

typedef struct {
    int         tpm_counter_ok;   /* 1 - значение счётчика прочитано и достоверно */
    uint64_t    counter;          /* значение счётчика (пол); имеет смысл при tpm_counter_ok */
    uint32_t    static_min;       /* вшитый нижний порог (0 - нет) */
    int         require_tpm;      /* KIRA_REQUIRE_TPM */
} rb_release_in_t;

/* Решение о выпуске image_version. RB_OK: загрузка разрешена, *src - чем защищена. */
int rb_release_decide(uint32_t image_version, const rb_release_in_t* in, rb_source_t* src);

/* Сколько раз увеличить счётчик, чтобы пол стал равен выпуску (но не больше RB_MAX_RAISE); 0 - не нужно. Пол НИКОГДА не выше выпуска. */
uint32_t rb_release_raise_steps(uint32_t image_version, uint64_t counter);

/* Полный порядок загрузчика над TPM (общий код загрузчика и хост-тестов): прочитать счётчик (создать при первом использовании TPM), решить.
   x == 0 - TPM нет. Результат: src - чем защищено, counter - значение (если читалось), created - счётчик создан сейчас, tpm_err - ошибка TPM2_E_*
   при недоступном счётчике (0 - не было). Возвращает RB_OK или причину отказа; при отказе *out заполнен для сообщения. */
typedef struct { rb_source_t src; uint64_t counter; int created; int tpm_err; } rb_release_result_t;
int rb_release_boot_check(tpm2_xfer_t x, void* ctx, uint32_t image_version, uint32_t static_min, int require_tpm, rb_release_result_t* out);

/* Подтверждение хорошей загрузки (ядро): поднять счётчик до выпуска, но не выше (rb_release_raise_steps). *after - значение после; 0 - успех,
   иначе TPM2_E_* (счётчик остаётся не выше выпуска). Выпуск ниже счётчика счётчик не меняет и не понижает: понизить его нельзя. */
int rb_release_commit(tpm2_xfer_t x, void* ctx, uint32_t image_version, uint64_t* after);

/* Сообщения (английский ASCII: выводятся до шрифтов). */
const char* rb_release_strerror(int rc);

/* ---- Откат диска ---- */
#define RB_ANCHOR_LEN 16u     /* размер ячейки якоря в TPM */

typedef enum {
    RB_DISK_OK = 0,           /* эпоха диска не меньше якоря */
    RB_DISK_NO_ANCHOR = 1,    /* якоря ещё нет (первая загрузка с TPM): принять эпоху диска как исходную */
    RB_DISK_ROLLBACK = 2,     /* эпоха диска меньше якоря: подставлена старая копия */
    RB_DISK_NO_TPM = 3        /* TPM недоступен: проверить нельзя (честно сообщить, не молчать) */
} rb_disk_verdict_t;

/* anchor_state: 1 - якорь прочитан, 0 - ячейки нет/пуста, -1 - TPM недоступен. */
rb_disk_verdict_t rb_disk_check(uint64_t disk_epoch, int anchor_state, uint64_t anchor);

/* Якорь можно обновить, только если новое значение больше текущего (понижать нельзя: иначе вернётся окно отката). 1 - писать, 0 - не нужно. */
int rb_anchor_should_write(int anchor_state, uint64_t anchor, uint64_t persisted_epoch);

/* Кодирование ячейки якоря: "KFSA", версия 1 (u32), эпоха (u64, старший байт первым). */
void rb_anchor_encode(uint64_t epoch, uint8_t out[RB_ANCHOR_LEN]);
int  rb_anchor_decode(const uint8_t in[RB_ANCHOR_LEN], uint64_t* epoch);   /* 0 - верно, иначе RB_E_ARGS */

/* Безопасный режим при откате диска: том только для чтения, пока оператор не подтвердит (команда fsanchor accept). */
#endif
