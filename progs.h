#ifndef KIRA_PROGS_H
#define KIRA_PROGS_H

#include <stdint.h>

typedef enum { PROG_REQUEST_ACCEPTED = 0, PROG_REQUEST_BUSY = -1, PROG_REQUEST_INVALID = -2 } prog_request_status_t;
typedef enum { PROG_RESULT_UNKNOWN = -1, PROG_RESULT_PENDING = 0, PROG_RESULT_COMPLETE = 1 } prog_result_state_t;

/* Запуск программ из файловой системы: /bin/<имя> -> SHA-256 -> иммунная проверка по манифесту сборки -> ELF -> процесс Ring 3.
   Диск трогает только основной поток ядра: другие контексты (дисплей) вызывают prog_request, а prog_poll выполняет запрос. */

int  prog_run(const char* name);        /* pid (>0) или отрицательный код: -1 нет файла/ошибка, -2 отказ иммунитета, -3 не принят как ELF */
prog_request_status_t prog_request(const char* name, uint32_t* request_id);
int  prog_run_args(const char* name, const char* args);   /* запуск с аргументами (run имя аргументы) */
prog_request_status_t prog_request_file(const char* name, const char* file, uint32_t* request_id);
prog_result_state_t prog_request_take_result(uint32_t request_id, int* result); /* COMPLETE consumes the result; called by the requester */
int  prog_run_local(const char* file);  /* runlocal: /data/kcc/<файл> как local-<имя> без прав (явная команда пользователя) */
void prog_poll(void);                   /* основной цикл ядра */
void prog_list(void);                   /* /bin с вердиктом доверия */

#endif
