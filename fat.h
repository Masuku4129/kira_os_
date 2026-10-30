#ifndef FAT_H
#define FAT_H
#include <stdint.h>
#include <stddef.h>
#include "blk.h"
#include "kfs.h"

/* FAT12/16/32 - файловая система флешек, карт памяти и дисков других систем.
   Чтение и запись: длинные имена (LFN, UTF-8), каталоги, создание/замена файла целиком, mkdir, удаление.
   Порядок записи щадящий: данные -> таблица FAT (все копии) -> запись каталога -> освобождение старых кластеров;
   отключение посреди записи оставляет в худшем случае потерянные кластеры (их найдёт fsck), а не испорченный файл.
   Ошибки - коды KFS_ERR_*, сведения о файле - kfs_stat_t: файловые системы для верхних слоёв одинаковы. */

#define FAT_SECTOR 512

typedef struct {
    blk_dev_t* dev;
    uint64_t base;                     /* первый сектор раздела на устройстве */
    uint32_t spc, rsvd, nfats, root_ents, fatsz, total, root_secs, data_start, clusters, root_clus;
    int      type;                     /* 12, 16, 32 */
    uint32_t fsinfo;                   /* сектор FSInfo (FAT32) или 0 */
    uint32_t free_count, next_free; int free_known;
    char     label[12];
    int      mounted;
    /* кэш одного сектора каталогов/данных и одного сектора таблицы FAT (с отложенной записью) */
    uint8_t  sec[FAT_SECTOR]; uint64_t sec_lba; int sec_valid;
    uint8_t  fsec[FAT_SECTOR]; uint32_t fsec_no; int fsec_valid, fsec_dirty;
    int      io_errors;
} fat_vol_t;

/* Часы для меток времени файлов: дата и время в формате FAT (если не задано - 1 января 2026, 00:00). */
void fat_set_clock(void (*now)(uint16_t* date, uint16_t* time));

int     fat_mount(fat_vol_t* v, blk_dev_t* dev, uint64_t base, uint64_t sectors);   /* 0 или KFS_ERR_NOFS/IO */
int     fat_stat(fat_vol_t* v, const char* path, kfs_stat_t* st);
int     fat_list(fat_vol_t* v, const char* path, kfs_list_cb cb, void* ctx);
int64_t fat_read(fat_vol_t* v, const char* path, uint64_t off, void* buf, size_t len);
int     fat_write(fat_vol_t* v, const char* path, const void* data, size_t len);     /* создать или заменить целиком */
int     fat_mkdir(fat_vol_t* v, const char* path);
int     fat_unlink(fat_vol_t* v, const char* path);                                 /* файл или пустой каталог */
void    fat_info(fat_vol_t* v, uint64_t* total_kb, uint64_t* free_kb);
int     fat_flush(fat_vol_t* v);

/* Чистые помощники (проверяются отдельно). */
int     fat_short_name(const char* name, uint8_t out[11], uint8_t* nt_case);       /* 1 - имя укладывается в 8.3 */
uint8_t fat_lfn_checksum(const uint8_t short83[11]);

#endif
