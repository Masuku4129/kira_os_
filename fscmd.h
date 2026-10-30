#ifndef KIRA_FSCMD_H
#define KIRA_FSCMD_H
/* Файловая система в консоли: монтирование при загрузке и команды ls/cat/write/rm/mkdir/df/format. */
void fs_init(void);
void fscmd_ls(const char* arg);
void fscmd_cat(const char* arg);
void fscmd_write(const char* arg);      /* "путь текст..." */
void fscmd_rm(const char* arg);
void fscmd_mkdir(const char* arg);
void fscmd_df(const char* arg);
void fscmd_format(const char* arg);     /* только "yes" */
void fscmd_cp(const char* arg);         /* "откуда куда" */
void fscmd_umount(const char* arg);     /* "usbN" */
#endif
