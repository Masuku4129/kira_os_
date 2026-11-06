#ifndef VFS_H
#define VFS_H
#include <stdint.h>
#include <stddef.h>
#include "kfs.h"
#include "fat.h"

/* Единое дерево файлов: корень - KiraFS системного диска, съёмные тома (FAT на флешках) - каталоги /usb0../usb3.
   Пути и коды ошибок - как у KiraFS. У каждого тома свой мьютекс: флешка и системный диск работают независимо;
   отключённый том перестаёт отвечать (KFS_ERR_NOFS), а не роняет вызывающего. */
#define VFS_MAX 64       /* /usb0 .. /usb63 */

typedef struct { char name[8]; char label[12]; char desc[48]; uint64_t total_kb, free_kb; int fat_type; int read_only; } vfs_info_t;

int      vfs_mount_fat(fat_vol_t* vol, const char* desc);    /* подключить с записью; номер тома или -1 */
int      vfs_mount_fat_ro(fat_vol_t* vol, const char* desc); /* подключить только для чтения (USB по умолчанию) */
void     vfs_unmount(int n);                                 /* том отключён (устройство вынули) */
int      vfs_unmount_try(int n);                             /* то же без ожидания: -1 - том сейчас занят */
int      vfs_flush(int n);                                   /* дописать отложенное перед извлечением */
uint32_t vfs_generation(void);                               /* растёт при подключении/отключении томов */
int      vfs_mounts(vfs_info_t* out, int max);               /* подключённые тома */

int      vfs_stat(const char* path, kfs_stat_t* st);
int      vfs_list(const char* path, kfs_list_cb cb, void* ctx);   /* в корне - ещё и каталоги томов */
int64_t  vfs_read(const char* path, uint64_t off, void* buf, size_t len);
int      vfs_write(const char* path, const void* data, size_t len);
int      vfs_mkdir(const char* path);
int      vfs_unlink(const char* path);
int      vfs_any(void);                                      /* есть хоть одна файловая система */

#endif
