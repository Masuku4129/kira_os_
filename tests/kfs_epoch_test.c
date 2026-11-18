/* Хост-тест эпохи тома KiraFS и безопасного режима (А3): эпоха растёт при записях и переживает перемонтирование, старая копия диска узнаётся по эпохе
   против якоря, в безопасном режиме том только читается, а до миграции (том без эпохи) всё работает как прежде. Якорь здесь - простая переменная:
   запись его в TPM и модель TPM проверяют tests/rollback_test.c. */
#include "kfs.h"
#include "rollback.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

#define SECT (16 + 4096)
static uint8_t disk[SECT * 512], old_copy[SECT * 512];
static int rd(blk_dev_t* d, uint64_t lba, uint32_t n, void* b){ (void)d; if(lba+n>SECT) return -1; memcpy(b, disk+lba*512, n*512); return 0; }
static int wr(blk_dev_t* d, uint64_t lba, uint32_t n, const void* b){ (void)d; if(lba+n>SECT) return -1; memcpy(disk+lba*512, b, n*512); return 0; }
static int fl(blk_dev_t* d){ (void)d; return 0; }
static blk_dev_t dev = {"ram", SECT, rd, wr, fl, 0};

int main(void){
    uint64_t e = 99;
    CHECK(kfs_format(&dev,16)==0 && kfs_mounted());
    CHECK(kfs_epoch()==0 && !kfs_epoch_dirty());
    CHECK(kfs_epoch_sync(&e)==0 && e==0);                      /* без изменений эпоха не растёт и на диск ничего не пишется */
    CHECK(kfs_write("/a","one",3)==0 && kfs_epoch_dirty());
    CHECK(kfs_epoch_sync(&e)==0 && e==1 && !kfs_epoch_dirty() && kfs_epoch()==1);
    CHECK(kfs_epoch_sync(&e)==0 && e==1);
    CHECK(kfs_mkdir("/d")==0 && kfs_write("/d/b","two",3)==0 && kfs_unlink("/d/b")==0);
    CHECK(kfs_epoch_sync(&e)==0 && e==2);                      /* много операций - одна эпоха на синхронизацию: износ не растёт с числом записей */
    memcpy(old_copy, disk, sizeof disk);                       /* «старая копия диска» на эпохе 2 */
    uint64_t anchor_old = e;
    CHECK(kfs_write("/a","three",5)==0 && kfs_epoch_sync(&e)==0 && e==3);
    CHECK(kfs_clone("/a","/c")==0 && kfs_epoch_sync(&e)==0 && e==4);
    uint64_t anchor = e;                                       /* якорь в TPM закреплён на 4 */
    /* перемонтирование: эпоха на диске сохранилась */
    CHECK(kfs_mount(&dev,16)==0 && kfs_epoch()==4);
    CHECK(rb_disk_check(kfs_epoch(), 1, anchor)==RB_DISK_OK);
    /* подмена старой копией */
    memcpy(disk, old_copy, sizeof disk);
    CHECK(kfs_mount(&dev,16)==0 && kfs_epoch()==2);
    CHECK(rb_disk_check(kfs_epoch(), 1, anchor)==RB_DISK_ROLLBACK);                       /* обнаружено: эпоха диска меньше якоря */
    CHECK(anchor_old == kfs_epoch());
    /* безопасный режим: только чтение */
    kfs_set_readonly(1);
    CHECK(kfs_readonly());
    uint8_t b[16];
    CHECK(kfs_read("/a",0,b,3)==3 && !memcmp(b,"one",3));                              /* читать можно */
    CHECK(kfs_write("/a","x",1)==KFS_ERR_RO && kfs_write("/new","x",1)==KFS_ERR_RO);
    CHECK(kfs_mkdir("/e")==KFS_ERR_RO && kfs_unlink("/a")==KFS_ERR_RO && kfs_clone("/a","/z")==KFS_ERR_RO);
    CHECK(kfs_write_packed("/p","x",1,1)==KFS_ERR_RO);
    CHECK(!kfs_epoch_dirty() && kfs_epoch_sync(&e)==0 && e==2);                           /* и эпоха не растёт */
    CHECK(kfs_read("/a",0,b,3)==3 && !memcmp(b,"one",3));                              /* отказавшие операции ничего не испортили */
    /* подтверждение оператора: том снова пишется, эпоха растёт от эпохи диска */
    kfs_set_readonly(0);
    CHECK(kfs_write("/a","y",1)==0 && kfs_epoch_sync(&e)==0 && e==3);
    /* повторное монтирование сбрасывает безопасный режим: решение принимает слой выше после сверки с якорем */
    kfs_set_readonly(1);
    CHECK(kfs_mount(&dev,16)==0 && !kfs_readonly());
    /* том без эпохи (созданный до А3): нули в области под эпоху, всё работает, эпоха стартует с 0 */
    CHECK(kfs_format(&dev,16)==0 && kfs_epoch()==0);
    CHECK(kfs_write("/x","1",1)==0 && kfs_epoch_sync(&e)==0 && e==1);
    /* суперблок с повреждённой эпохой не принимается (CRC) */
    disk[16 * 512 + 300] ^= 1;
    CHECK(kfs_mount(&dev,16)==KFS_ERR_CORRUPT);
    printf("kfs epoch: все проверки пройдены\n");
    return 0;
}
