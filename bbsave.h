#ifndef BBSAVE_H
#define BBSAVE_H

#include <stdint.h>
#include "bbring.h"

/* Чёрный ящик на диске (кольцо - bbring.c). Файл /var/log/blackbox.bin - кольцо текущего запуска; при первом сбросе нового запуска
   прежний файл становится /var/log/blackbox.old.bin (два файла, размер каждого не больше 48 КиБ + заголовок). Запись - только из
   отдельного потока «blackbox», из консоли и перед выключением; цикл дисплея, планировщик и прерывания диска не касаются. */

void bbsave_init(void);                       /* после монтирования KiraFS: время и ядро для записей, номер запуска, поток сброса */
int  bbsave_now(unsigned reason);             /* сбросить кольцо на диск сейчас (BB_SAVE_*); 0 - записано */
void bbsave_core_tick(void);                  /* раз за проход основного цикла ядра: разрыв между проходами - событие «пауза» */
void bbsave_command(const char* arg);         /* консоль: bb [N|all] | bb old | bb save | bb clear | bb pause мс */
unsigned bbsave_status(char* out, unsigned cap);   /* строка для fb: когда и почему сброшено, номер запуска */

#endif
