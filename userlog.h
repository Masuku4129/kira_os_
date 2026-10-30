#ifndef USERLOG_H
#define USERLOG_H

#include <stdint.h>
#include <stddef.h>

/* Вывод программ в общий журнал. Строка собирается отдельно для каждого процесса и выводится целиком, с префиксом
   "[USER pid N] ", когда пришёл перевод строки: программы на разных ядрах и программа, вытесненная посреди строки,
   не рвут чужие и свои строки. Длинная строка выводится кусками по USERLOG_LINE знаков (у каждого куска префикс).
   Незаконченная строка завершившегося процесса выводится userlog_flush. */
#define USERLOG_SLOTS 16
#define USERLOG_LINE  160

typedef struct { uint32_t pid; uint32_t len; uint32_t age; char buf[USERLOG_LINE]; } userlog_slot_t;
typedef struct { userlog_slot_t slot[USERLOG_SLOTS]; uint32_t clock; } userlog_t;

typedef void (*userlog_emit_fn)(const char* line);    /* целая строка с префиксом и '\n', оканчивается нулём */

void userlog_write(userlog_t* st, uint32_t pid, const char* b, size_t n, userlog_emit_fn emit);
void userlog_flush(userlog_t* st, uint32_t pid, userlog_emit_fn emit);

#endif
