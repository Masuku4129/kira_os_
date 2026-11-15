/* COW-копии KiraFS (kfs_clone): общие блоки, счёт ссылок, замена и удаление одной из копий, перезапуск, обрыв питания. */
#include "kfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)
#define SECT (16 + 16384)
static uint8_t disk[SECT * 512];
static long budget = -1;
static int rd(blk_dev_t* d, uint64_t lba, uint32_t n, void* b){ (void)d; if(lba+n>SECT) return -1; memcpy(b, disk+lba*512, n*512); return 0; }
static int wr(blk_dev_t* d, uint64_t lba, uint32_t n, const void* b){
    (void)d; if(lba+n>SECT) return -1;
    for(uint32_t i=0;i<n;i++){ if(budget==0) return -1; memcpy(disk+(lba+i)*512,(const uint8_t*)b+i*512,512); if(budget>0) budget--; }
    return 0;
}
static int fl(blk_dev_t* d){ (void)d; return 0; }
static blk_dev_t dev = {"ram", SECT, rd, wr, fl, 0};
static uint8_t buf[300000], chk[300000];
static void fill(size_t n, int seed){ for(size_t i=0;i<n;i++) buf[i]=(uint8_t)(seed*31+i*7+(i>>8)); }
static int has(const char* p, size_t n, int seed){
    kfs_stat_t st; if(kfs_stat(p,&st)||st.size!=n) return 0;
    fill(n,seed); if(kfs_read(p,0,chk,n+5)!=(int64_t)n) return 0;
    return memcmp(buf,chk,n)==0;
}
static uint32_t freeb(void){ kfs_info_t i; kfs_info(&i); return i.free_blocks; }

int main(void){
    CHECK(kfs_format(&dev,16)==0);
    fill(10,1); CHECK(kfs_write("/w",buf,10)==0 && kfs_unlink("/w")==0);   /* блок корневого каталога занят навсегда */
    uint32_t f0 = freeb();
    fill(200000,7); CHECK(kfs_write("/a",buf,200000)==0);       /* 49 блоков: косвенный */
    uint32_t f1 = freeb();
    CHECK(kfs_clone("/a","/b")==0);
    CHECK(freeb()==f1);                                          /* данные не копировались */
    kfs_info_t in; kfs_info(&in); CHECK(in.shared==1);
    CHECK(has("/a",200000,7) && has("/b",200000,7));
    CHECK(kfs_clone("/a","/b")==KFS_ERR_EXIST && kfs_clone("/nope","/c")==KFS_ERR_NOENT);
    CHECK(kfs_mkdir("/d")==0 && kfs_clone("/d","/e")==KFS_ERR_ISDIR);
    CHECK(kfs_clone("/a","/d/c")==0);                            /* три ссылки */
    uint32_t f2 = freeb();                                       /* с блоком каталога /d */
    /* перезапуск: ссылки восстанавливаются по инодам */
    CHECK(kfs_mount(&dev,16)==0);
    kfs_info(&in); CHECK(in.shared==1 && in.repaired==0 && freeb()==f2);
    CHECK(has("/b",200000,7) && has("/d/c",200000,7));
    /* замена одной копии не трогает остальные */
    fill(5000,9); CHECK(kfs_write("/b",buf,5000)==0);
    CHECK(has("/a",200000,7) && has("/d/c",200000,7) && has("/b",5000,9));
    /* удаление: пока есть копия, блоки живы */
    uint32_t fa = freeb();
    CHECK(kfs_unlink("/a")==0 && freeb()==fa && has("/d/c",200000,7));
    kfs_info(&in); CHECK(in.shared==0);                          /* остался один владелец - обычный файл */
    CHECK(kfs_unlink("/d/c")==0 && freeb()>fa);                  /* последний - блоки возвращены */
    CHECK(kfs_unlink("/b")==0 && kfs_unlink("/d")==0 && freeb()==f0);
    CHECK(kfs_mount(&dev,16)==0 && freeb()==f0);
    /* пустой файл копируется без общих блоков */
    CHECK(kfs_write("/z",buf,0)==0 && kfs_clone("/z","/z2")==0); kfs_info(&in); CHECK(in.shared==0);
    CHECK(kfs_unlink("/z")==0 && kfs_unlink("/z2")==0);

    /* обрыв питания на каждой записи clone: оригинал цел, копия либо есть целиком, либо нет, место сходится после очистки */
    fill(60000,3); CHECK(kfs_write("/s",buf,60000)==0);
    static uint8_t snap[sizeof disk]; memcpy(snap,disk,sizeof disk);
    uint32_t fs0 = freeb();
    for(long cut=0; cut<40; cut++){
        memcpy(disk,snap,sizeof disk); CHECK(kfs_mount(&dev,16)==0);
        budget=cut; int r=kfs_clone("/s","/t"); budget=-1;
        CHECK(kfs_mount(&dev,16)==0);
        CHECK(has("/s",60000,3));
        kfs_stat_t st; int t=kfs_stat("/t",&st)==0;
        if(r==0) CHECK(t);
        if(t) CHECK(has("/t",60000,3));
        if(t) CHECK(kfs_unlink("/t")==0);
        CHECK(has("/s",60000,3) && freeb()==fs0);
    }
    printf("kfs cow: clone, refs, replace, unlink, remount, power-cut sweep passed\n");
    return 0;
}
