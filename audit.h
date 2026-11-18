#ifndef KIRA_AUDIT_H
#define KIRA_AUDIT_H
/* Журнал аудита в ядре (docs/AUDIT.md): кольцо событий (audit_event, auditev.h) -> поток audit: печать в цепочку SHA-256, подписанные контрольные точки
   Ed25519 ключом устройства, сегменты /var/audit*.log на KiraFS (auditstore.c). Ключ создаётся при первой загрузке и лежит в /var/audit.key: программы
   до /var не дотягиваются (их песочница - /data/<имя>). Чтение программами - системный вызов SYS_AUDIT с правом CAP_AUDIT. */
#include <stdint.h>
#include "auditev.h"

void audit_init(void);                 /* из kernel_main после bbsave_init: поток журнала; события до готовности ФС ждут в кольце */
void audit_command(const char* arg);   /* команда консоли audit: (пусто|N) | status | verify | export [с [сколько]] | key | checkpoint */
void audit_shutdown(void);             /* штатное выключение/перезагрузка: запись shutdown, подписанная точка и сброс на диск прямо сейчас */
void audit_sync(void);                 /* то же без записи shutdown (сон, сохранение перед выключением кэшей): всё принятое - в цепочку, точка, диск */
/* SYS_AUDIT: записи (по 160 байт) с номера from в буфер пользователя; не больше 8 за вызов. Результат - число записей, 0 - больше нет, <0 - ошибка (KE_*). */
int64_t audit_sys_read(uint64_t from_seq, uint64_t user_buf, uint64_t cap);
#endif
