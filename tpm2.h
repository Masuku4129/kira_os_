#ifndef KIRA_TPM2_H
#define KIRA_TPM2_H
/* Чистая часть работы с TPM 2.0: сборка команд и разбор ответов для энергонезависимой памяти (NV): счётчик и обычная ячейка.
   Нужна и UEFI-загрузчику (команды уходят через TCG2 SubmitCommand), и ядру (драйвер TIS, tpmtis.c), и хост-тестам, поэтому без libc, без
   выделения памяти и без обращения к устройству: передачу байтов делает вызывающий через tpm2_xfer_t. Применение - docs/SECURE_BOOT.md, А3:
   монотонный счётчик номера выпуска образа (защита от отката образа) и якорь эпохи диска (защита от подмены диска старой копией).

   Права доступа. Оба NV-индекса создаются с правами владельца (TPMA_NV_OWNERWRITE и OWNERREAD) и без атрибута ORDERLY (иначе счётчик
   сохранялся бы только при штатном выключении и терялся при обрыве питания). Команды идут с сеансом парольной авторизации (TPM_RS_PW) и ПУСТЫМ
   паролем владельца: так TPM выглядит сразу после очистки. Если владелец задал пароль, TPM вернёт отказ авторизации (TPM2_RC_AUTH), и код это
   различает: честное сообщение «задайте пароль в конфигурации» вместо молчаливого отказа от защиты. Пароль хранить в загрузчике и ядре нельзя. */
#include <stdint.h>
#include <stddef.h>

#define TPM2_ST_NO_SESSIONS   0x8001u
#define TPM2_ST_SESSIONS      0x8002u
#define TPM2_CC_NV_DEFINE     0x0000012Au
#define TPM2_CC_NV_WRITE      0x00000137u
#define TPM2_CC_NV_INCREMENT  0x00000134u
#define TPM2_CC_NV_READ       0x0000014Eu
#define TPM2_RH_OWNER         0x40000001u
#define TPM2_RS_PW            0x40000009u
#define TPM2_ALG_SHA256       0x000Bu

/* Атрибуты NV (TPMA_NV): тип в битах 4..7; без ORDERLY и без PPWRITE; NO_DA - перебор пароля не запирает индекс. */
#define TPM2_NV_OWNERWRITE    0x00000002u
#define TPM2_NV_NT_COUNTER    0x00000010u
#define TPM2_NV_OWNERREAD     0x00020000u
#define TPM2_NV_NO_DA         0x02000000u
#define TPM2_NV_ATTR_COUNTER  (TPM2_NV_OWNERWRITE | TPM2_NV_NT_COUNTER | TPM2_NV_OWNERREAD | TPM2_NV_NO_DA)
#define TPM2_NV_ATTR_ORDINARY (TPM2_NV_OWNERWRITE | TPM2_NV_OWNERREAD | TPM2_NV_NO_DA)

/* Индексы KIRA в области, отведённой владельцу (0x01500000..0x01BFFFFF). */
#define KIRA_NV_RELEASE_COUNTER 0x015A4B01u   /* монотонный счётчик номера выпуска образа (8 байт, только +1) */
#define KIRA_NV_DISK_ANCHOR     0x015A4B02u   /* якорь эпохи диска (обычная ячейка 16 байт: tpm2_anchor_* в rollback.h) */

#define TPM2_MAX_CMD  64u      /* самая длинная команда этого модуля - 52 байта */
#define TPM2_MAX_RSP  64u

enum {
    TPM2_OK         = 0,
    TPM2_E_ARGS     = -1,    /* неверные аргументы, буфер мал */
    TPM2_E_IO       = -2,    /* передача команды не удалась (нет устройства, тайм-аут) */
    TPM2_E_FORMAT   = -3,    /* ответ короче заголовка, размер не сходится, неожиданный тег */
    TPM2_E_RC       = -4,    /* TPM вернул ненулевой код (он в *rc) */
    TPM2_E_ABSENT   = -5,    /* NV-индекса нет (TPM2_RC_HANDLE) */
    TPM2_E_EMPTY    = -6,    /* индекс определён, но ни разу не записан и не увеличен (TPM2_RC_NV_UNINITIALIZED) */
    TPM2_E_AUTH     = -7,    /* отказ авторизации: у владельца задан пароль или индекс закрыт */
    TPM2_E_LOCKED   = -8     /* TPM в режиме защиты от перебора (TPM2_RC_LOCKOUT) */
};

/* Передача: команду cmd[cmd_len] отдать TPM, ответ - в rsp; *rsp_len на входе - размер буфера, на выходе - длина ответа. 0 - обмен удался. */
typedef int (*tpm2_xfer_t)(void* ctx, const uint8_t* cmd, size_t cmd_len, uint8_t* rsp, size_t* rsp_len);

/* Сборка команд: возвращают длину или 0 (буфер мал). Данные NV - не длиннее 64 байт. */
size_t tpm2_cmd_nv_define(uint32_t index, uint32_t attrs, uint16_t size, uint8_t* out, size_t cap);
size_t tpm2_cmd_nv_increment(uint32_t index, uint8_t* out, size_t cap);
size_t tpm2_cmd_nv_read(uint32_t index, uint16_t size, uint8_t* out, size_t cap);
size_t tpm2_cmd_nv_write(uint32_t index, const uint8_t* data, uint16_t len, uint8_t* out, size_t cap);

/* Разбор ответа: заголовок и размер проверяются строго. 0 - код ответа в *rc; для успешного NV_Read данные копируются в data (len байт ровно). */
int tpm2_rsp_rc(const uint8_t* rsp, size_t len, uint32_t* rc);
int tpm2_rsp_nv_data(const uint8_t* rsp, size_t len, uint8_t* data, uint16_t want);
/* Код ответа -> TPM2_E_*: TPM2_OK для 0. */
int tpm2_rc_class(uint32_t rc);

/* Операции поверх передачи: сборка, обмен, разбор. *rc (может быть 0) получает код ответа TPM. */
int tpm2_nv_define(tpm2_xfer_t x, void* ctx, uint32_t index, uint32_t attrs, uint16_t size, uint32_t* rc);
int tpm2_nv_increment(tpm2_xfer_t x, void* ctx, uint32_t index, uint32_t* rc);
int tpm2_nv_read(tpm2_xfer_t x, void* ctx, uint32_t index, uint8_t* data, uint16_t len, uint32_t* rc);
int tpm2_nv_write(tpm2_xfer_t x, void* ctx, uint32_t index, const uint8_t* data, uint16_t len, uint32_t* rc);
/* Значение счётчика (8 байт, старший первым) через tpm2_nv_read. */
int tpm2_nv_counter_read(tpm2_xfer_t x, void* ctx, uint32_t index, uint64_t* value, uint32_t* rc);

const char* tpm2_strerror(int err);
#endif
