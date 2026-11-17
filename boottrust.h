#ifndef KIRA_BOOTTRUST_H
#define KIRA_BOOTTRUST_H
/* Состояние доверенной загрузки, как его передал загрузчик (BootInfo: trust_flags и image_version, bootinfo.h; А2, docs/SECURE_BOOT.md).
   Ядро ничего здесь не проверяет заново: подпись и хэши проверил загрузчик до передачи управления. Модуль хранит и честно сообщает,
   что именно было проверено, чтобы журнал, команда secboot и дальнейшие подсистемы (А3: откат, А7: журнал аудита) опирались на факты,
   а не на предположение «загрузились, значит доверенно». Чистые функции без зависимостей: проверяются хост-тестом. */
#include <stdint.h>
#include "bootinfo.h"

typedef struct {
    uint32_t flags;           /* KBI_TRUST_* */
    uint32_t image_version;   /* номер выпуска из подписанного манифеста; достоверен только при KBI_TRUST_SIG_VERIFIED */
    int      reported;        /* 1 - загрузчик передал сведения (UEFI); 0 - BIOS, GRUB, дерево устройств: цепочки доверия нет */
} boottrust_t;

/* Запомнить сведения из заголовка BootInfo (вызывается один раз, до kernel_main); h == 0 - загрузчик без сведений. */
void boottrust_init(const kbi_header_t* h);
const boottrust_t* boottrust_get(void);

/* Печать состояния построчно через out (uart_print): строки вида «[KIRA BOOT] ...». Возвращает число строк. */
int boottrust_report(const boottrust_t* t, void (*out)(const char*), void (*out_dec)(uint64_t));

/* 1 - загрузка доверенная в полном смысле: Secure Boot включён, загрузчик принуждающий и подпись ядра проверена. */
int boottrust_is_trusted(const boottrust_t* t);
#endif
