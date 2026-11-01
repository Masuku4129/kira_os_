#ifndef KIRA_PROGS_H
#define KIRA_PROGS_H

/* Запуск программ из файловой системы: /bin/<имя> -> SHA-256 -> иммунная проверка по манифесту сборки -> ELF -> процесс Ring 3.
   Диск трогает только основной поток ядра: другие контексты (дисплей) вызывают prog_request, а prog_poll выполняет запрос. */

int  prog_run(const char* name);        /* pid (>0) или отрицательный код: -1 нет файла/ошибка, -2 отказ иммунитета, -3 не принят как ELF */
void prog_request(const char* name);
int  prog_run_args(const char* name, const char* args);   /* запуск с аргументами (run имя аргументы) */
void prog_request_file(const char* name, const char* file);   /* программа с правом читать файл file */    /* из любого потока: запуск будет выполнен в prog_poll */
void prog_poll(void);                   /* основной цикл ядра */
void prog_list(void);                   /* /bin с вердиктом доверия */

#endif
