#ifndef KIRA_BOOTSIG_H
#define KIRA_BOOTSIG_H
/* Подписанный манифест загрузки (файл \EFI\KIRA\KIRA.SIG на ESP): доверенная загрузка по цепочке UEFI Secure Boot (docs/SECURE_BOOT.md).
   Манифест перечисляет файлы, которые загрузчик вправе передать ядру (ядро и, если есть, initrd), с их размерами и SHA-256, и несёт
   номер выпуска образа для защиты от отката (А3). Подписывает его Ed25519-ключ выпуска; открытый ключ вшит в загрузчик, закрытый
   лежит вне образа. Чистая часть без libc и без выделения памяти: одинаково собирается в UEFI-загрузчик (GNU-EFI), ядро и хост-тесты.

   Раскладка (всё little-endian), размер = 32 + 56 * N + 64:
     0   8   "KIRASIG1"
     8   4   image_version - монотонный номер выпуска (откат ниже нижнего порога отклоняется)
     12  4   flags - 0 (ненулевое отклоняется: неизвестное требование нельзя игнорировать)
     16  4   count - число записей 1..BOOTSIG_MAX_ENTRIES; запись 0 - всегда ядро
     20  12  резерв, нули
     32  56*N записи: name[16] (ASCII, добивка нулями), size u64, sha256[32]
     ... 64  подпись Ed25519 над (BOOTSIG_DOMAIN || все байты до подписи)
   Доменный префикс отделяет эти подписи от любых других подписей тем же ключом. */
#include <stdint.h>
#include <stddef.h>

#define BOOTSIG_MAGIC        "KIRASIG1"
#define BOOTSIG_DOMAIN       "KIRA-BOOT-MANIFEST-V1\0"      /* 22 байта вместе с нулём */
#define BOOTSIG_DOMAIN_LEN   22u
#define BOOTSIG_HEADER_LEN   32u
#define BOOTSIG_ENTRY_LEN    56u
#define BOOTSIG_SIG_LEN      64u
#define BOOTSIG_MAX_ENTRIES  4u
#define BOOTSIG_MAX_LEN      (BOOTSIG_HEADER_LEN + BOOTSIG_ENTRY_LEN * BOOTSIG_MAX_ENTRIES + BOOTSIG_SIG_LEN)
#define BOOTSIG_NAME_KERNEL  "KIRA.ELF"
#define BOOTSIG_NAME_INITRD  "INITRD.IMG"

enum {
    BOOTSIG_OK          = 0,
    BOOTSIG_E_ARGS      = -1,    /* неверные аргументы вызова */
    BOOTSIG_E_FORMAT    = -2,    /* не тот размер, магия, флаги, число записей, имена */
    BOOTSIG_E_SIGNATURE = -3,    /* подпись не сходится с вшитым ключом */
    BOOTSIG_E_ROLLBACK  = -4,    /* номер выпуска ниже нижнего порога */
    BOOTSIG_E_UNLISTED  = -5,    /* файла нет в манифесте */
    BOOTSIG_E_SIZE      = -6,    /* размер файла не совпал с манифестом */
    BOOTSIG_E_HASH      = -7     /* SHA-256 файла не совпал с манифестом (файл изменён) */
};

typedef struct {
    char     name[16];           /* ASCII, с нулём */
    uint64_t size;
    uint8_t  sha256[32];
} bootsig_entry_t;

typedef struct {
    uint32_t        image_version;
    uint32_t        count;
    bootsig_entry_t entry[BOOTSIG_MAX_ENTRIES];
} bootsig_t;

/* Разбор и проверка подписи манифеста. Подпись проверяется ДО того, как поля попадают в out; при любой ошибке out обнуляется.
   pub - 32 байта открытого ключа Ed25519, min_version - нижний порог номера выпуска (0 - без проверки отката). */
int bootsig_parse_verify(const uint8_t* blob, size_t len, const uint8_t pub[32], uint32_t min_version, bootsig_t* out);

/* Сверка файла с манифестом: имя должно быть в списке, размер и SHA-256 - совпасть. */
int bootsig_check_file(const bootsig_t* s, const char* name, const void* data, size_t len);

/* Запись с таким именем или 0 (для обязательных и лишних файлов). */
const bootsig_entry_t* bootsig_find(const bootsig_t* s, const char* name);

/* Сообщение для экрана и COM1 (английский текст, ASCII: выводится до загрузки шрифтов и кодировок). */
const char* bootsig_strerror(int rc);

/* Сборка подписываемого сообщения: домен + заголовок + записи. Нужна тестам и инструментам; возвращает длину или 0 при ошибке.
   Само подписание делает внешний инструмент (ключ вне образа). */
size_t bootsig_message(const bootsig_t* s, uint8_t* out, size_t cap);
#endif
