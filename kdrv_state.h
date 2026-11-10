#ifndef KIRA_KDRV_STATE_H
#define KIRA_KDRV_STATE_H
/* Постоянное состояние защиты драйверов Ring 3 от отката: /var/kdrv_state (KiraFS), один файл, замена целиком (kfs_write атомарна).
   Внутри: (1) пол версий по имени драйвера, (2) максимально виденное время, (3) отозванные издательские ключи.
   Чистая логика без обращений к ядру: ядро (kdrv.c) только читает и пишет файл, хост-тест проверяет все ветки.

   Что это даёт и чего НЕ даёт. Файл лежит на том же диске, что и пакеты. Кто может записать на диск (снять его, подменить образ
   целиком), тот откатит и пол, и время, и отзывы: без аппаратной опоры (TPM NV-счётчик, RPMB eMMC/NVMe, Secure Boot с
   монотонным счётчиком в прошивке) от этого программно не защититься. Защита рассчитана на: подмену одного пакета (.kdrv/.sig/.rec),
   ошибку установки старой версии, сдвиг часов без доступа к диску, компрометацию одного ключа издателя. */
#include <stddef.h>
#include <stdint.h>
#include "kdrv_format.h"
#include "kdrv_sig.h"

#define KDRV_STATE_PATH        "/var/kdrv_state"
#define KDRV_STATE_MAGIC       0x3154534Bu      /* "KST1" */
#define KDRV_STATE_FORMAT      1u
#define KDRV_STATE_MAX_FLOORS  32u
#define KDRV_STATE_MAX_REVOKED 16u
#define KDRV_STATE_NAME_SIZE   16u
/* заголовок 24 + полы 32*20 + ключи 16*16 + SHA-256 32 */
#define KDRV_STATE_FILE_SIZE   952u
/* Часы могут уйти назад не больше чем на сутки (исправление часового пояса, подстройка RTC); больше - отказ. Внутри допуска время
   считается не меньше максимально виденного, так что подпись, которая уже истекла, не оживает. */
#define KDRV_CLOCK_TOLERANCE   86400
/* Максимум времени пишется на диск не чаще раза в час (износ носителя); при перезагрузке теряется не больше часа - меньше допуска. */
#define KDRV_TIME_PERSIST_STEP 3600

enum { KDRV_STATE_OK = 0, KDRV_STATE_CORRUPT = 1, KDRV_STATE_IO = 2 };
enum { KDRV_STATE_E_FULL = -1, KDRV_STATE_E_ARG = -2, KDRV_STATE_E_CORRUPT = -3 };

typedef struct { char name[KDRV_STATE_NAME_SIZE]; uint32_t floor; } kdrv_floor_t;

typedef struct {
    uint64_t max_time;                                   /* максимально виденное время UTC */
    uint32_t nfloor, nrevoked;
    kdrv_floor_t floor[KDRV_STATE_MAX_FLOORS];
    uint8_t revoked[KDRV_STATE_MAX_REVOKED][KDRV_SIG_KEY_ID_SIZE];
    /* ниже в файл не пишется */
    uint64_t persisted_time;                             /* max_time, уже лежащий на диске */
    int status;                                          /* KDRV_STATE_*; не OK - драйверы не принимаются, тихого сброса нет */
    int dirty;                                           /* есть что записать */
} kdrv_state_t;

void kdrv_state_init(kdrv_state_t* st);                  /* пустое состояние (файла ещё не было), status OK */
int  kdrv_state_decode(kdrv_state_t* st, const void* buf, size_t len);   /* 0; иначе состояние пустое, status CORRUPT */
void kdrv_state_mark_io_error(kdrv_state_t* st);         /* файл есть, но прочитать не удалось: status IO */
void kdrv_state_encode(const kdrv_state_t* st, uint8_t out[KDRV_STATE_FILE_SIZE]);

uint32_t kdrv_state_floor_of(const kdrv_state_t* st, const char* name);    /* 0 - пола нет */
int  kdrv_state_raise_floor(kdrv_state_t* st, const char* name, uint32_t version);   /* 1 поднят, 0 уже не ниже, <0 ошибка; понижать нельзя */
int  kdrv_state_revoke_key(kdrv_state_t* st, const uint8_t key_id[KDRV_SIG_KEY_ID_SIZE]);   /* 1 добавлен, 0 уже был, <0 ошибка */
int  kdrv_state_is_revoked(const kdrv_state_t* st, const uint8_t key_id[KDRV_SIG_KEY_ID_SIZE]);
void kdrv_state_reset_time(kdrv_state_t* st, uint64_t now);   /* ЯВНАЯ команда пользователя: максимум := now (часы были заведены вперёд по ошибке) */

/* Решение о допуске пакета - ровно то, что вызывает ядро при каждой загрузке:
   1) состояние читаемо, иначе KDRV_E_STATE;
   2) если есть подпись (или она обязательна): время не ушло назад (KDRV_SIG_E_CLOCK), отозванные ключи, срок ключа и подписи,
      хеш и Ed25519 (KDRV_E_SIGNATURE, причина - *sig_error);
   3) версия из заголовка пакета (она входит в хеш, который подписан) не ниже пола (KDRV_E_ROLLBACK).
   Заголовок разбирается заново из pkg: версия берётся из тех же байт, что подписаны. sig == 0 - подписи нет.
   Время фиксируется в состоянии при каждой проверке подписи, в том числе отказной: иначе после истечения подписи часы можно
   было бы вернуть, пока ничего не записало новый максимум. */
int kdrv_state_verify(kdrv_state_t* st, const uint8_t domain[16], const void* data, size_t len, const void* sig, size_t sig_len,
                      const kdrv_sig_key_t* keys, uint32_t nkeys, int64_t now_raw);
int kdrv_admit(kdrv_state_t* st, const void* pkg, size_t pkg_len, const void* sig, size_t sig_len, int sig_required,
               const kdrv_sig_key_t* keys, uint32_t nkeys, int64_t now_raw,
               kdrv_header_t* h_out, int* auth, int* sig_error);

#endif
