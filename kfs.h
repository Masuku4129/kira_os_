#ifndef KFS_H
#define KFS_H
#include <stdint.h>
#include <stddef.h>
#include "blk.h"

/* KiraFS v2 - файловая система Киры. Иерархия каталогов, блок 4 КБ, размер тома, файлов и каталогов определяет
   диск (подробно - kfs.c).
   Расчёт на обрыв питания без журнала:
   - каждое изменение метаданных - запись ОДНОГО сектора (инода или записи каталога), а сектор пишется целиком;
   - данные пишутся в новые блоки и сбрасываются на диск ДО того, как иноду переключают на них, поэтому файл
     после сбоя - либо целиком старый, либо целиком новый;
   - карта занятых блоков на диске не хранится: при монтировании она строится по инодам, заодно подбираются
     «потерянные» иноды и блоки (результат незавершённых операций) и убираются записи каталога с битыми ссылками.
   Каждый инод защищён CRC32. Операции - под мьютексом файловой системы. */

#define KFS_BLOCK     4096
#define KFS_NAME_MAX  59
#define KFS_BASE_LBA  16          /* сектора 0..15 заняты хранилищем опыта (store.c) */

enum { KFS_FILE = 1, KFS_DIR = 2 };
enum {
    KFS_OK = 0, KFS_ERR_IO = -1, KFS_ERR_NOENT = -2, KFS_ERR_EXIST = -3, KFS_ERR_NOTDIR = -4, KFS_ERR_ISDIR = -5,
    KFS_ERR_NOSPC = -6, KFS_ERR_INVAL = -7, KFS_ERR_NOTEMPTY = -8, KFS_ERR_FBIG = -9, KFS_ERR_CORRUPT = -10,
    KFS_ERR_NAMETOOLONG = -11, KFS_ERR_NOFS = -12
};

typedef struct { uint32_t type; uint64_t size; uint32_t ino; } kfs_stat_t;
typedef struct { uint32_t total_blocks, free_blocks, inodes, free_inodes, repaired; } kfs_info_t;   /* блоки по 4 КБ */
typedef int (*kfs_list_cb)(void* ctx, const char* name, const kfs_stat_t* st);   /* ненулевой результат прерывает обход; внутри нельзя вызывать другие функции kfs_* (общие рабочие буферы) */

int  kfs_detect(blk_dev_t* dev, uint64_t base_lba);   /* 0 - на диске корректная KiraFS (только чтение, без монтирования) */
int  kfs_format(blk_dev_t* dev, uint64_t base_lba);   /* создаёт пустую систему (стирает область!) и монтирует её */
int  kfs_mount(blk_dev_t* dev, uint64_t base_lba);    /* 0 - готово; KFS_ERR_NOFS - на диске нет KiraFS */
int  kfs_mounted(void);
int  kfs_stat(const char* path, kfs_stat_t* st);
int64_t kfs_read(const char* path, uint64_t off, void* buf, size_t len);   /* прочитано байт или ошибка */
int  kfs_write(const char* path, const void* data, size_t len);            /* создаёт или целиком и атомарно заменяет файл */
int  kfs_mkdir(const char* path);
int  kfs_unlink(const char* path);                    /* файл или пустой каталог */
int  kfs_list(const char* path, kfs_list_cb cb, void* ctx);
void kfs_info(kfs_info_t* out);
void kfs_set_run_max(uint32_t blocks);   /* предел сплошного участка в одном запросе к диску (16..256 блоков) */
const char* kfs_strerror(int err);

#endif
