#ifndef KIRA_DRVGET_API_H
#define KIRA_DRVGET_API_H
/* Логика drvget как библиотека для окна «Драйверы» (userland/drvman.c). Собирается тот же userland/drvget.c с -DDRVGET_LIB:
   скачивание по HTTPS, проверка лицензии и хеша, сборка kcc на устройстве, запись в /drv - один и тот же код, что у консольной
   команды drvget; отличается только место вывода текста (drvget_set_sink) и то, что подтверждение даёт нажатие кнопки, а не ввод хеша.
   Вызовы блокирующие (сеть, компиляция): окно вызывает их из рабочего потока. */
#include <stddef.h>
#include <stdint.h>
#include "drvget_core.h"

void drvget_set_sink(void (*f)(const char *text, size_t len));    /* весь текст отчётов и ошибок идёт сюда (из потока, который вызвал функцию) */
int  drvget_find(const char *name_or_url);                        /* = drvget install: скачать, проверить, показать; ничего не ставит. 0 - набор готов к установке */
const dg_recipe_t *drvget_staged(void);                           /* рецепт подготовленного набора или 0 */
const char *drvget_staged_url(void);                              /* адрес рецепта подготовленного набора */
const char *drvget_staged_hash(void);                             /* SHA-256 подготовленного набора (64 знака) или пустая строка */
int  drvget_install_staged(void);                                 /* = drvget confirm ИМЯ ПОЛНЫЙ_ХЕШ для набора, показанного последним find */
int  drvget_remove(const char *name);                             /* удалить пакет из /drv вместе с подтверждением */
int  drvget_verify(const char *name);                             /* = drvget verify ИМЯ: хеши образа и пакета против записи */
int  drvget_list(void);                                           /* = drvget list (текстом) */
int  drvget_base(const char *url);                                /* каталог рецептов (пусто - показать текущий) */
int  drvget_ctl(int start, const char *name);                     /* 1 - запустить, 0 - остановить: просьба менеджеру драйверов ядра (FS_OP_DRV_START/STOP); 0 - принято */

enum { DGP_OK = 0, DGP_BAD = 1, DGP_NOREC = 2, DGP_UNREADABLE = 3, DGP_TOOBIG = 4,
       DGP_UNCONF = 5 };
enum { DGP_AUTH_UNKNOWN = 0, DGP_AUTH_LOCAL = 1, DGP_AUTH_PUBLISHER = 2, DGP_AUTH_INVALID = 3 };                /* запись есть, но пользователь установку не подтверждал: не запустится */
typedef struct {
    char name[16];
    uint32_t version, caps, caps_ok, nmatch;
    int state;                          /* состояние локальной записи и хешей */
    int auth_state;                     /* DGP_AUTH_PUBLISHER - подпись издателя проверена; LOCAL - подписи нет; INVALID - отклонена */
    kdrv_match_t match[KDRV_MATCH_MAX];
    uint8_t elf_sha[32], src_sha[32];
    char source[96];
    char license[24];                   /* пусто - в записи не указана (пакет установлен без метки лицензии) */
} drvget_pkg_t;
int drvget_pkgs(drvget_pkg_t *out, int max);                      /* установленные пакеты /drv; число или -1, если каталога нет */

int drvget_match_pci(uint16_t vendor, uint16_t device, char *name, size_t name_cap, char *url, size_t url_cap);
#endif
