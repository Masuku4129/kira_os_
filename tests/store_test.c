#include "store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

static uint8_t disk[64 * 512];
static int fail_next_write, fail_flush;
static unsigned write_calls;
static int rd(blk_dev_t* d, uint64_t lba, uint32_t n, void* b){ (void)d; if(lba+n>64) return -1; memcpy(b, disk+lba*512, n*512); return 0; }
static int wr(blk_dev_t* d, uint64_t lba, uint32_t n, const void* b){ (void)d; write_calls++; if(fail_next_write){ fail_next_write=0; memcpy(disk+lba*512, b, 20); return -1; } memcpy(disk+lba*512, b, n*512); return 0; }
static int fl(blk_dev_t* d){ (void)d; return fail_flush ? -1 : 0; }

int main(void){
    CHECK(crc32("123456789", 9) == 0xCBF43926u);
    blk_dev_t dev = {"ram", 64, rd, wr, fl, 0};
    store_t s; experience_t e, g;
    memset(disk, 0, sizeof disk);
    CHECK(store_mount(&s, &dev, &e) == 0 && e.boot_count == 0);       /* пустой диск */
    memset(&e, 0, sizeof e); e.boot_count = 1; e.load_ema_x100 = 300; e.qcount = 1; e.q[0].size = 77; memcpy(e.q[0].reason, "nop-sled", 9);
    blk_dev_t noflush = dev; noflush.flush = 0; store_t sn;
    CHECK(store_mount(&sn, &noflush, &g) == 0);
    unsigned writes_before = write_calls;
    CHECK(store_commit(&sn, &e) == -2 && write_calls == writes_before); /* never report a commit without a flush barrier */
    CHECK(store_commit(&s, &e) == 0);
    e.boot_count = 2; CHECK(store_commit(&s, &e) == 0);
    e.boot_count = 3; CHECK(store_commit(&s, &e) == 0);
    store_t s2; CHECK(store_mount(&s2, &dev, &g) == 1);
    CHECK(g.boot_count == 3 && g.load_ema_x100 == 300 && g.q[0].size == 77 && !strcmp(g.q[0].reason, "nop-sled") && s2.generation == 3);
    /* порча новейшей ячейки (поколение 3 лежит в LBA 1): откат на поколение 2 */
    disk[1*512 + 100] ^= 0x55;
    CHECK(store_mount(&s2, &dev, &g) == 1 && g.boot_count == 2 && s2.generation == 2);
    /* следующая запись идёт в испорченную ячейку и становится поколением 3 */
    e.boot_count = 9; CHECK(store_commit(&s2, &e) == 0);
    CHECK(store_mount(&s2, &dev, &g) == 1 && g.boot_count == 9 && s2.generation == 3);
    store_t exhausted = s2; exhausted.generation = UINT64_MAX; writes_before = write_calls;
    CHECK(store_commit(&exhausted, &e) == -3 && write_calls == writes_before); /* no generation wrap */
    /* оборванная запись: устройство вернуло ошибку - прошлое состояние цело */
    fail_next_write = 1; e.boot_count = 10; CHECK(store_commit(&s2, &e) == -1);
    CHECK(s2.generation == 3);
    CHECK(store_mount(&s2, &dev, &g) == 1 && g.boot_count == 9);
    fail_flush = 1; CHECK(store_commit(&s2, &e) == -2); fail_flush = 0;
    /* обе ячейки испорчены -> «чистая жизнь», без падения */
    disk[0*512 + 40] ^= 1; disk[1*512 + 40] ^= 1;
    CHECK(store_mount(&s2, &dev, &g) == 0 && g.boot_count == 0);
    /* неизвестная версия отвергается */
    memset(disk, 0, sizeof disk); store_mount(&s, &dev, &e); e.boot_count = 5; store_commit(&s, &e);
    disk[1*512 + 8] = 2;
    CHECK(store_mount(&s2, &dev, &g) == 0);
    /* ошибка чтения устройства */
    blk_dev_t bad = {"bad", 0, rd, wr, fl, 0};
    CHECK(store_mount(&s2, &bad, &g) == -1);
    CHECK(store_detect(&bad) == 0);
    printf("store: CRC vector, A/B generations, torn write, corruption fallback passed\n");
    return 0;
}
