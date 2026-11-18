#ifndef KIRA_APPMAN_H
#define KIRA_APPMAN_H
/* Подписанный манифест приложений (А3, docs/APP_ISOLATION.md, docs/V44_SECURITY_MODEL.md): список «имя программы -> SHA-256 образа», который выпускает
   издатель и подписывает своим ключом Ed25519. Лежит на диске двумя файлами: /etc/apps.manifest (этот формат) и /etc/apps.sig (конверт kdrv_sig_t с
   доменом KDRV_SIG_DOMAIN_APPS: тот же ключ издателя, что у драйверов, приложений и печати системы, но подпись одного назначения не годится для другого).
   Подпись проверяет ядро (kdrv_apps_sig_check, kdrv.c) тем же путём, что подписи драйверов: часы не ушли назад, ключ не отозван (флаг в таблице ядра или
   `drv revoke`), срок ключа и подписи, - то есть ротация и отзыв ключей общие со всей системой, отдельного списка отзыва для приложений не заводится.

   Зачем при встроенном в ядро манифесте SHA-256 (trust.h). Встроенный манифест защищает от подмены файлов на диске относительно образа ядра, но не связывает
   программы с издателем и меняется только пересборкой ядра. Подписанный манифест можно выпустить и обновить без пересборки ядра, а в доверенном режиме
   (boottrust.h) он ОБЯЗАТЕЛЕН: без верной подписи ни одна программа из /bin не запускается. В режиме разработки поведение прежнее (встроенный манифест).

   Раскладка файла (little-endian): 0 8 "KIRAAPPS"; 8 4 serial - порядковый номер выпуска манифеста; 12 4 count 1..APPMAN_MAX; 16 4 flags = 0;
   20 12 нули; затем count записей по 56 байт: name[24] (ASCII без пробелов, добивка нулями, уникальные) и sha256[32]. Размер = 32 + 56 * count. */
#include <stdint.h>
#include <stddef.h>
#include "trust.h"

#define APPMAN_MAGIC     "KIRAAPPS"
#define APPMAN_HEADER    32u
#define APPMAN_ENTRY     56u
#define APPMAN_NAME_LEN  24u
#define APPMAN_MAX       256u
#define APPMAN_MAX_LEN   (APPMAN_HEADER + APPMAN_ENTRY * APPMAN_MAX)
#define APPMAN_PATH      "/etc/apps.manifest"
#define APPMAN_SIG_PATH  "/etc/apps.sig"

enum { APPMAN_OK = 0, APPMAN_E_ARGS = -1, APPMAN_E_FORMAT = -2 };

/* Режим допуска программ. */
typedef enum {
    APPMAN_DEV = 0,      /* встроенный манифест ядра (SHA-256), подпись не требуется: как всегда */
    APPMAN_SIGNED = 1    /* допуск только по манифесту с верной подписью издателя */
} appman_mode_t;

/* trusted - boottrust_is_trusted(); dev_flag - строка ядра kira.signedapps (только в режиме разработки: в доверенном режиме строка не нужна и не принимается). */
appman_mode_t appman_mode(int trusted, int dev_flag);

/* Проверка формата (подпись здесь не проверяется: её проверяет ядро над теми же байтами раньше). 0 - верно. */
int appman_parse(const uint8_t* blob, size_t len, uint32_t* serial, uint32_t* count);

/* Вердикт по имени и SHA-256 образа: TRUST_OK, TRUST_TAMPERED (имя есть, образ другой), TRUST_UNKNOWN (имени нет). Кривой формат - TRUST_UNKNOWN. */
trust_verdict_t appman_check(const uint8_t* blob, size_t len, const char* name, const uint8_t digest[32]);

/* Сборка файла из записей (для инструментов и тестов); возвращает длину или 0. names - ASCII без пробелов, не длиннее 23 символов. */
size_t appman_build(uint32_t serial, const trust_entry_t* entries, unsigned n, uint8_t* out, size_t cap);

const char* appman_strerror(int rc);
#endif
