// Сохранение и загрузка рисунка в Paint: настоящий код программы против настоящего файлового сервиса.
#include "stub/kira.h"        /* занимает защиту KIRA_USER_H: настоящий userland/kira.h не подключается */
#include "kfs.h"
#include <stdlib.h>
#define umain paint_main
#include "../userland/paint.c"
#define CHECK(c) do{ if(!(c)){ printf("FAIL line %d: %s\n",__LINE__,#c); exit(1);} }while(0)

char stub_log[4096];
#define SECT (16 + 4096)
static uint8_t disk[SECT * 512];
static int rd(blk_dev_t* d, uint64_t l, uint32_t n, void* b) { (void)d; memcpy(b, disk + l * 512, n * 512); return 0; }
static int wr(blk_dev_t* d, uint64_t l, uint32_t n, const void* b) { (void)d; memcpy(disk + l * 512, b, n * 512); return 0; }
static int fl(blk_dev_t* d) { (void)d; return 0; }
static blk_dev_t dev = { "ram", SECT, rd, wr, fl, 0 };
static u32 px[W * H], snap[W * H];

int main(void) {
    CHECK(kfs_format(&dev, 16) == 0);
    for (int i = BAR * W; i < W * H; i++) px[i] = COLORS[7];
    srand(3);
    for (int s = 0; s < 120; s++) stroke(px, rand() % W, BAR + rand() % (H - BAR), rand() % W, BAR + rand() % (H - BAR), COLORS[rand() % 7]);
    memcpy(snap, px, sizeof px);

    stub_log[0] = 0; save_picture(px);
    CHECK(strstr(stub_log, "saved"));
    kfs_stat_t st; CHECK(kfs_stat("/data/paint/picture.kpnt", &st) == 0 && st.size > 8 && st.size < MAX_FILE);
    for (int i = BAR * W; i < W * H; i++) px[i] = 0x00123456;                  /* затираем и загружаем */
    stub_log[0] = 0; CHECK(load_picture(px) == 1 && strstr(stub_log, "loaded"));
    CHECK(memcmp(px + BAR * W, snap + BAR * W, (size_t)W * (H - BAR) * 4) == 0);

    /* повреждённые файлы отвергаются целиком, рисунок не меняется */
    static u8 bad[200000];
    const u32 total = (u32)W * (H - BAR);
    memcpy(snap, px, sizeof px);
    struct { u8 data[40]; u32 len; } cases[8]; memset(cases, 0, sizeof cases);
    int nc = 0;
    memcpy(cases[nc].data, "KPNT", 4); cases[nc++].len = 4;                                                   /* короткий */
    memcpy(cases[nc].data, "XXXX\xE0\x01\x28\x01\x08\x00\x00\x00", 12); cases[nc++].len = 12;                  /* не та метка */
    memcpy(cases[nc].data, "KPNT\xE1\x01\x28\x01\x01\x00\x00\x00", 12); cases[nc++].len = 12;                  /* не та ширина */
    memcpy(cases[nc].data, "KPNT\xE0\x01\x28\x01\x09\xFF\xFF", 11); cases[nc++].len = 11;                     /* цвет 9 */
    memcpy(cases[nc].data, "KPNT\xE0\x01\x28\x01\x01\x00\x00", 11); cases[nc++].len = 11;                     /* нулевая длина */
    memcpy(cases[nc].data, "KPNT\xE0\x01\x28\x01\x01\x10\x00\x02", 12); cases[nc++].len = 12;                 /* не хватает пикселей */
    for (int c = 0; c < nc; c++) {
        CHECK(k_file_write(FILE_NAME, cases[c].data, cases[c].len) == 0);
        stub_log[0] = 0; CHECK(load_picture(px) == 0 && (strstr(stub_log, "bad") || strstr(stub_log, "no saved")));
        CHECK(memcmp(px, snap, sizeof px) == 0);
    }
    /* серия, которая не покрывает рисунок целиком (длина обрезана до 16 бит), отвергается */
    memcpy(bad, "KPNT\xE0\x01\x28\x01", 8); u32 n = 8;
    bad[n] = 1; bad[n + 1] = (u8)(total & 255); bad[n + 2] = (u8)((total >> 8) & 255); n += 3;
    CHECK((total & 0xFFFF) != total && k_file_write(FILE_NAME, bad, n) == 0);
    stub_log[0] = 0; CHECK(load_picture(px) == 0 && strstr(stub_log, "bad") && memcmp(px, snap, sizeof px) == 0);

    /* слишком сложный рисунок не сохраняется и старый файл остаётся */
    save_picture(snap);                                                                                        /* исходный файл - предыдущая картинка */
    for (int i = 0; i < (int)total; i++) px[BAR * W + i] = COLORS[(i + (i / W)) % 2 ? 1 : 2];                  /* шахматный узор: 2 байта на пиксель - не влезет */
    stub_log[0] = 0; save_picture(px);
    CHECK(strstr(stub_log, "too complex"));
    CHECK(load_picture(px) == 1);

    printf("paint: save/load round-trip, strict validation of corrupt files, oversize refusal passed\n");
    return 0;
}
