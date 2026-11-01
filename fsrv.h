#ifndef KIRA_FSRV_H
#define KIRA_FSRV_H
#include <stdint.h>
#include <stddef.h>

/* Файловый сервис для программ Ring 3. У каждой программы одна песочница: каталог /data/<имя программы>.
   Программа называет файлы только по короткому имени внутри песочницы (без '/', '..' и абсолютных путей), поэтому
   выйти за её пределы нельзя по построению. Имя программы известно ядру из проверенного запуска (progs.c: хеш по манифесту),
   а не со слов самой программы. Запрос выполняется в потоке самой программы (KiraFS и диск - под мьютексами). */

#define FSRV_NAME_MAX  40
/* Пределы определяет машина: один запрос - до четверти свободной ОЗУ (данные копируются ядром), файлов у программы -
   до 1/8 таблицы инодов (она растёт вместе с диском), места - пока на диске остаётся 1/16 тома системе. */
uint64_t fsrv_max_data(void);
uint64_t fsrv_max_files(void);

enum { FS_OP_READ = 1, FS_OP_WRITE = 2, FS_OP_STAT = 3, FS_OP_LIST = 4, FS_OP_REMOVE = 5,
       FS_OP_GRANT_READ = 6, FS_OP_GRANT_STAT = 7, FS_OP_GRANT_NAME = 8,
       FS_OP_SYS_READ = 9, FS_OP_SYS_STAT = 10,
       FS_OP_INC_READ = 11, FS_OP_INC_STAT = 12 };   /* /etc/kcc/include/<имя>: заголовки libc-lite, только чтение и только для компилятора kcc */   /* /etc/ssl/<имя>: общие файлы системы, только чтение (корневые сертификаты) */   /* выданный при запуске файл (вне песочницы) */
enum { FS_E_DENIED = -100, FS_E_ARG = -101, FS_E_QUOTA = -102 };    /* прочие отрицательные значения - коды kfs.h */

typedef struct {                        /* то, что программа передаёт в системный вызов (48 байт) */
    uint32_t op, name_len;
    uint64_t name_ptr, buf_ptr, buf_len, off;
} fsrv_user_req_t;

typedef struct {
    uint32_t op;
    char prog[16];
    char name[FSRV_NAME_MAX + 1];
    uint64_t off, len;                  /* READ: смещение и сколько читать; WRITE: сколько байт в data */
    int64_t result;
    uint8_t* data;                      /* буфер вызывающего на len байт (LIST - на len байт списка) */
} fsrv_req_t;

int  fsrv_prog_ok(const char* prog);    /* [a-z0-9_-], до 15 символов */
int  fsrv_name_ok(const char* name);    /* [A-Za-z0-9._-], первый символ буква или цифра, до 40 */
/* Имя процесса для собранной пользователем программы /data/kcc/<file>: «local-» + имя файла до первой точки (строчные буквы,
   цифры, '-', '_'; всего до 15 знаков). 1 - готово, 0 - имя не подходит. Песочница у такой программы своя: /data/local-<...>. */
int  fsrv_local_name(const char* file, char out[16]);
int  fsrv_sandbox(const char* prog);    /* создаёт /data/<prog>; 0 - готово */
void fsrv_execute(fsrv_req_t* r);       /* выполняет запрос на KiraFS, результат в r->result */

int64_t fsrv_syscall(uint64_t user_req_ptr);   /* контекст системного вызова процесса */

#endif
