#ifndef KIRA_AUDITEV_H
#define KIRA_AUDITEV_H
/* Журнал аудита (docs/AUDIT.md): виды событий и вход для записи события. Заголовок без зависимостей: его включают любые уровни
   (сторож, часы, драйверы, консоль, сеть), а запись, цепочку хэшей и подпись делает audit.c. До инициализации журнала события
   складываются в кольцо в памяти и попадают на диск, когда ФС готова. Вызов безопасен из прерывания и из паники: только замок и память. */
#include <stdint.h>

enum {                              /* что произошло */
    AUD_K_BOOT = 1,                 /* запуск системы: arg - биты AUD_BOOT_*, detail - состояние доверенной загрузки */
    AUD_K_CKPT = 2,                 /* подписанная контрольная точка (пишет сам журнал) */
    AUD_K_LOSS = 3,                 /* события не вошли в кольцо (диск не успевал/не писался): arg - сколько, субъект - причина */
    AUD_K_PRUNE = 4,                /* удалены самые старые закрытые сегменты: arg - последний удалённый номер записи */
    AUD_K_KEY = 5,                  /* ключ устройства создан (arg 0) или заменён (arg 1) */
    AUD_K_EXEC = 6,                 /* запуск программы: субъект - имя, arg - права (caps), result - принят ли запуск */
    AUD_K_CAP_DENY = 7,             /* отказ по правам (первый на процесс): arg - право, detail - номер вызова */
    AUD_K_DRV_INSTALL = 8,          /* установка драйвера (пакет записан/принят) */
    AUD_K_DRV_REMOVE = 9,           /* удаление драйвера, отзыв ключа, сброс состояния защиты */
    AUD_K_RCON_PASSWD = 10,         /* смена пароля удалённой консоли */
    AUD_K_RCON_LOGIN = 11,          /* вход в rcon: result OK/DENIED; arg - подряд неверных попыток */
    AUD_K_MOUNT = 12,               /* монтирование тома (KiraFS, USB-FAT) */
    AUD_K_CONFIG = 13,              /* изменение конфигурации (rcon on/off, пол версий драйверов...) */
    AUD_K_TRUST = 14,               /* состояние доверенной загрузки (Secure Boot, измерения), как увидело ядро */
    AUD_K_SLEEP = 15,               /* уход в сон S3 */
    AUD_K_WAKE = 16,                /* пробуждение */
    AUD_K_PANIC = 17,               /* паника ядра */
    AUD_K_WATCHDOG = 18,            /* сторож: предупреждение, остановка, паника */
    AUD_K_IMMUNE = 19,              /* иммунная система: программа отклонена или помещена в карантин */
    AUD_K_TIME = 20,                /* смена времени: arg - новое время в секундах (младшие 32 бита), detail - источник */
    AUD_K_SHUTDOWN = 21,            /* штатное выключение или перезагрузка */
    AUD_K_TAMPER = 22,              /* при проверке найдено нарушение журнала: arg - номер записи, detail - причина */
    AUD_K_RCON_CMD = 23,            /* команда, принятая по rcon (кроме смены пароля) */
    AUD_K_MAX = 24
};
enum { AUD_SRC_KERNEL = 0, AUD_SRC_CONSOLE = 1, AUD_SRC_RCON = 2, AUD_SRC_PROGRAM = 3, AUD_SRC_SYSTEM = 4, AUD_SRC_MAX = 5 };   /* кто */
enum { AUD_R_OK = 0, AUD_R_DENIED = 1, AUD_R_FAIL = 2, AUD_R_MAX = 3 };                                                           /* результат */
enum { AUD_BOOT_UNCLEAN = 1, AUD_BOOT_TAMPER = 2, AUD_BOOT_NEWKEY = 4 };

/* subject - кто/что (имя программы, пользователь, устройство), detail - подробности; обе строки усекаются (24 и 56 байт) и могут быть 0. */
void audit_event(unsigned kind, unsigned src, unsigned res, uint32_t pid, uint32_t dom, uint32_t arg, const char* subject, const char* detail);
/* То же от имени текущего процесса (pid, домен, имя из proc.c) - для мест, где вызов идёт из программы. */
void audit_event_cur(unsigned kind, unsigned res, uint32_t arg, const char* detail);
/* Источник команды консоли, выполняемой сейчас (консоль или rcon): ставит console.c; события, вызванные командой, берут его. */
void audit_set_cmd_source(unsigned src);
unsigned audit_cmd_source(void);

#endif
