#include "kfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

#define SECT (16 + 16384)
#define BIGF (5u * 1024 * 1024 + 123)          /* дважды косвенные блоки (v1 позволял до ~4 МБ) */
#define MAXSECT (16 + 4 * 16384)              /* диск можно «увеличить» */
static uint64_t cur_sect = SECT;
static uint8_t disk[MAXSECT * 512];
static long budget = -1;       /* сколько записей ещё дойдёт до диска; -1 - без ограничений */
static long writes;
static int rd(blk_dev_t* d, uint64_t lba, uint32_t n, void* b){ (void)d; if(lba+n>cur_sect) return -1; memcpy(b, disk+lba*512, n*512); return 0; }
static int wr(blk_dev_t* d, uint64_t lba, uint32_t n, const void* b){
    (void)d; if(lba+n>cur_sect) return -1;
    for(uint32_t i=0;i<n;i++){                       /* сектор пишется целиком или не пишется (потеря питания) */
        if(budget==0) return -1;
        memcpy(disk+(lba+i)*512, (const uint8_t*)b+i*512, 512); writes++;
        if(budget>0) budget--;
    }
    return 0;
}
static int fl(blk_dev_t* d){ (void)d; return 0; }
static blk_dev_t dev = {"ram", SECT, rd, wr, fl, 0};
static void set_size(uint64_t sect){ cur_sect = sect; dev.sectors = sect; }

static uint8_t pat(int seed, size_t i){ return (uint8_t)(seed * 31 + i * 7 + (i >> 8)); }
static void fill(uint8_t* b, size_t n, int seed){ for(size_t i=0;i<n;i++) b[i]=pat(seed,i); }
static uint8_t buf[BIGF + 64];
static int same(const char* p, size_t n, int seed){
    kfs_stat_t st; if(kfs_stat(p,&st)) return 0;
    if(st.type!=KFS_FILE || st.size!=n) return 0;
    int64_t r = kfs_read(p,0,buf,n+10); if(r!=(int64_t)n) return 0;
    for(size_t i=0;i<n;i++) if(buf[i]!=pat(seed,i)) return 0;
    return 1;
}
static int put(const char* p, size_t n, int seed){ fill(buf,n,seed); return kfs_write(p,buf,n); }

static int count_cb(void* c, const char* n, const kfs_stat_t* s){ (void)n;(void)s; (*(int*)c)++; return 0; }
static int count(const char* p){ int c=0; CHECK(kfs_list(p,count_cb,&c)==0); return c; }

static void base_state(void){
    memset(disk,0,sizeof disk); budget=-1;
    CHECK(kfs_format(&dev,16)==0);
    CHECK(put("/a",6000,1)==0);
    CHECK(kfs_mkdir("/d")==0);
    CHECK(put("/d/b",50000,2)==0);      /* 13 блоков: используется косвенный */
}

int main(void){
    /* Devices without a persistence barrier must be rejected before any filesystem writes. */
    memset(disk,0,sizeof disk);
    blk_dev_t noflush = dev; noflush.flush = 0;
    long noflush_writes = writes;
    CHECK(kfs_format(&noflush,16)==KFS_ERR_IO);
    CHECK(writes==noflush_writes && !kfs_mounted());
    CHECK(kfs_mount(&noflush,16)==KFS_ERR_IO);
    CHECK(writes==noflush_writes && !kfs_mounted());

    /* базовые операции */
    CHECK(kfs_mount(&dev,16)==KFS_ERR_NOFS);
    CHECK(kfs_format(&dev,16)==0 && kfs_mounted());
    kfs_info_t in0; kfs_info(&in0);
    CHECK(put("/hello",100,3)==0 && same("/hello",100,3));
    CHECK(put("/empty",0,0)==0 && same("/empty",0,0));
    CHECK(put("/big",BIGF,4)==0 && same("/big",BIGF,4));
    CHECK(kfs_read("/big",4500000,buf,1000)==1000); for(int i=0;i<1000;i++) CHECK(buf[i]==pat(4,4500000+i));
    CHECK(kfs_mount(&dev,16)==0 && same("/big",BIGF,4));                     /* дерево указателей переживает монтирование */
    CHECK(kfs_mkdir("/x")==0 && kfs_mkdir("/x")==KFS_ERR_EXIST);
    CHECK(kfs_mkdir("/x/y")==0 && put("/x/y/f",5000,5)==0 && same("/x/y/f",5000,5));
    CHECK(put("/nodir/f",1,1)==KFS_ERR_NOENT && put("/hello/f",1,1)==KFS_ERR_NOTDIR);
    CHECK(kfs_unlink("/x")==KFS_ERR_NOTEMPTY && kfs_unlink("/x/y/f")==0 && kfs_unlink("/x/y")==0 && kfs_unlink("/x")==0);
    CHECK(kfs_unlink("/x")==KFS_ERR_NOENT && kfs_unlink("/")==KFS_ERR_INVAL);
    CHECK(kfs_write("/",buf,1)==KFS_ERR_ISDIR && kfs_write("relative",buf,1)==KFS_ERR_INVAL);
    CHECK(kfs_stat("/a/../b",&(kfs_stat_t){0})==KFS_ERR_INVAL);
    char longn[100]; longn[0]='/'; memset(longn+1,'n',80); longn[81]=0; CHECK(put(longn,1,1)==KFS_ERR_NAMETOOLONG);
    /* перезапись меньшим и большим размером, возврат блоков */
    kfs_info_t a,b; kfs_unlink("/big"); kfs_unlink("/empty"); kfs_unlink("/hello");
    kfs_info(&a);
    for(int i=0;i<20;i++){ CHECK(put("/r",(size_t)(i%7)*9000+1,i)==0 && same("/r",(size_t)(i%7)*9000+1,i)); }
    CHECK(kfs_unlink("/r")==0); kfs_info(&b);
    CHECK(a.free_blocks==b.free_blocks && a.free_inodes==b.free_inodes);
    CHECK(in0.free_blocks >= a.free_blocks);
    /* частичное чтение */
    CHECK(put("/p",10000,9)==0);
    CHECK(kfs_read("/p",4090,buf,20)==20); for(int i=0;i<20;i++) CHECK(buf[i]==pat(9,4090+i));
    CHECK(kfs_read("/p",10000,buf,5)==0 && kfs_read("/p",9990,buf,50)==10);
    /* каталог растёт за пределы одного блока (64 записи) */
    CHECK(kfs_mkdir("/many")==0);
    char nm[32];
    for(int i=0;i<150;i++){ snprintf(nm,sizeof nm,"/many/f%03d",i); if(i<59 || 1){ int r=put(nm,10,i); if(r){ CHECK(r==KFS_ERR_NOSPC); break; } } }
    int cnt=count("/many"); CHECK(cnt>=60);
    snprintf(nm,sizeof nm,"/many/f%03d",cnt-1); CHECK(same(nm,10,cnt-1));
    /* пересборка: перемонтирование даёт то же самое без «ремонта» */
    kfs_info(&a); CHECK(kfs_mount(&dev,16)==0); kfs_info(&b);
    CHECK(b.repaired==0 && a.free_blocks==b.free_blocks && a.free_inodes==b.free_inodes && same("/p",10000,9));
    /* заполнение диска до отказа и освобождение */
    CHECK(kfs_format(&dev,16)==0);
    int n=0; for(;;n++){ snprintf(nm,sizeof nm,"/fill%d",n); int r=put(nm,30000,n); if(r){ CHECK(r==KFS_ERR_NOSPC); break; } }
    CHECK(n>=3);
    for(int i=0;i<n;i++){ snprintf(nm,sizeof nm,"/fill%d",i); CHECK(same(nm,30000,i)); }
    for(int i=0;i<n;i++){ snprintf(nm,sizeof nm,"/fill%d",i); CHECK(kfs_unlink(nm)==0); }
    CHECK(put("/again",30000,1)==0);

    /* обрыв питания на каждой записи каждой операции */
    int total_cases=0;
    for(int op=0; op<6; op++){
        base_state();
        long w0=writes; budget=-1;
        /* считаем, сколько записей делает операция */
        switch(op){
        case 0: CHECK(put("/a",20000,11)==0); break;
        case 1: CHECK(put("/d/c",9000,12)==0); break;
        case 2: CHECK(kfs_unlink("/d/b")==0); break;
        case 3: CHECK(kfs_mkdir("/d/sub")==0); break;
        case 4: CHECK(put("/d/b",3000,13)==0); break;
        case 5: CHECK(put("/a",BIGF,14)==0); break;                 /* дважды косвенные блоки */
        }
        long need=writes-w0;
        CHECK(need>0 && (need<200 || op==5));
        for(long k=0;k<=need;k += (op==5 && k < need-64) ? 97 : 1){
            base_state(); budget=k;
            int r;
            switch(op){
            case 0: r=put("/a",20000,11); break;
            case 1: r=put("/d/c",9000,12); break;
            case 2: r=kfs_unlink("/d/b"); break;
            case 3: r=kfs_mkdir("/d/sub"); break;
            case 4: r=put("/d/b",3000,13); break;
            default: r=put("/a",BIGF,14); break;
            }
            if(k<need) CHECK(r!=0);
            budget=-1;                                    /* питание вернулось */
            CHECK(kfs_mount(&dev,16)==0);
            kfs_info_t i1; kfs_info(&i1);
            switch(op){
            case 0: CHECK(same("/a",6000,1) || same("/a",20000,11)); break;
            case 5: CHECK(same("/a",6000,1) || same("/a",BIGF,14)); break;
            case 1: { kfs_stat_t st; if(kfs_stat("/d/c",&st)==0) CHECK(same("/d/c",9000,12)); } break;
            case 2: { kfs_stat_t st; if(kfs_stat("/d/b",&st)==0) CHECK(same("/d/b",50000,2)); } break;
            case 3: break;
            case 4: CHECK(same("/d/b",50000,2) || same("/d/b",3000,13)); break;
            }
            CHECK(same("/a",6000,1) || op==0 || op==5);
            if(op!=2 && op!=4) CHECK(same("/d/b",50000,2));
            /* повторный запуск ничего не чинит и не меняет счётчиков */
            CHECK(kfs_mount(&dev,16)==0);
            kfs_info_t i2; kfs_info(&i2);
            CHECK(i2.repaired==0 && i1.free_blocks==i2.free_blocks && i1.free_inodes==i2.free_inodes);
            /* система живёт дальше */
            CHECK(put("/after",12345,7)==0 && same("/after",12345,7));
            CHECK(kfs_unlink("/after")==0);
            total_cases++;
        }
    }
    printf("crash cases: %d\n", total_cases);

    /* порча метаданных: монтирование не падает, дальнейшее чтение безопасно */
    srand(12345);
    int mounted_ok=0, rejected=0;
    for(int it=0; it<400; it++){
        base_state();
        int flips = 1 + rand()%4;
        for(int f=0; f<flips; f++){
            size_t region = (16 + rand()%(1+8*3)) * 512 + rand()%512;   /* суперблок, таблица инодов, первые блоки данных */
            if(it%3==0) region = (16+8)*512 + rand()%1024;               /* таблица инодов */
            disk[region] ^= (uint8_t)(1 + rand()%255);
        }
        int r=kfs_mount(&dev,16);
        if(r){ rejected++; CHECK(!kfs_mounted()); continue; }
        mounted_ok++;
        const char* paths[]={"/","/a","/d","/d/b"};
        for(int p=0;p<4;p++){ kfs_stat_t st; if(kfs_stat(paths[p],&st)==0){ if(st.type==KFS_FILE) (void)kfs_read(paths[p],0,buf,sizeof buf); else (void)count(paths[p]); } }
        (void)put("/z",8000,1); (void)kfs_unlink("/z");
        kfs_info_t i; kfs_info(&i); CHECK(i.free_blocks<=i.total_blocks);
    }
    printf("fuzz: mounted %d, rejected %d\n", mounted_ok, rejected);

    /* v2: каталог больше 12 блоков (768 записей), рост таблицы инодов, рост тома вместе с диском */
    memset(disk,0,sizeof disk); set_size(SECT);
    CHECK(kfs_format(&dev,16)==0);
    kfs_info_t g0; kfs_info(&g0);
    CHECK(kfs_mkdir("/many")==0);
    int made=0;
    for(int i=0;i<1500;i++){ snprintf(nm,sizeof nm,"/many/n%04d",i); int r=put(nm,1,i); if(r){ CHECK(r==KFS_ERR_NOSPC); break; } made++; }
    kfs_info_t g1; kfs_info(&g1);
    CHECK(made==1500 && g1.inodes > g0.inodes && count("/many")==1500);     /* иноды кончились - таблица выросла сама */
    CHECK(same("/many/n0000",1,0) && same("/many/n1499",1,1499));
    CHECK(kfs_mount(&dev,16)==0); kfs_info(&g1);
    CHECK(g1.repaired==0 && count("/many")==1500 && same("/many/n0777",1,777));
    set_size(MAXSECT);                                                     /* диск стал вчетверо больше */
    CHECK(kfs_mount(&dev,16)==0); kfs_info_t g2; kfs_info(&g2);
    CHECK(g2.total_blocks == (MAXSECT-16)/8 && g2.free_blocks > g1.free_blocks + 3*16384/8*7/8 && same("/many/n1499",1,1499));
    CHECK(put("/big",BIGF,9)==0 && put("/big2",BIGF,10)==0 && same("/big",BIGF,9) && same("/big2",BIGF,10));
    for(int i=0;i<1500;i++){ snprintf(nm,sizeof nm,"/many/n%04d",i); CHECK(kfs_unlink(nm)==0); }
    CHECK(kfs_unlink("/many")==0 && kfs_unlink("/big")==0 && kfs_unlink("/big2")==0);
    kfs_info_t g3; kfs_info(&g3);
    CHECK(kfs_mount(&dev,16)==0); kfs_info_t g4; kfs_info(&g4);
    CHECK(g4.repaired==0 && g3.free_blocks==g4.free_blocks && g3.free_inodes==g4.free_inodes);
    /* том v1 (старая раскладка) монтируется и переходит на v2 без потери данных */
    {
        extern int v1_make(uint8_t* disk, uint64_t sectors);
        memset(disk,0,sizeof disk); set_size(SECT);
        CHECK(v1_make(disk, SECT)==0);
        CHECK(kfs_mount(&dev,16)==0 && same("/old/file",3000000,21) && same("/hello",5,22));
        CHECK(put("/old/bigger",BIGF,23)==0 && same("/old/bigger",BIGF,23));
        CHECK(kfs_mount(&dev,16)==0); kfs_info_t v; kfs_info(&v); CHECK(v.repaired==0 && same("/old/file",3000000,21));
    }
    printf("kfs v2: 1500-entry directory, inode table growth, volume growth with the disk, 5 MB files, v1 upgrade passed\n");
    printf("kfs ok\n");
    return 0;
}
