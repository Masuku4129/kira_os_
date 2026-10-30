/* Эталон KiraFS v1 (как в v28 и раньше): создаёт том старого формата, чтобы проверить, что v2 его монтирует
   и переводит на новый суперблок без потери данных. Публичные имена переименованы, чтобы жить рядом с v2. */
#include <string.h>
#include <stdint.h>
#define kfs_detect  v1_kfs_detect
#define kfs_mount   v1_kfs_mount
#define kfs_format  v1_kfs_format
#define kfs_mounted v1_kfs_mounted
#define kfs_stat    v1_kfs_stat
#define kfs_read    v1_kfs_read
#define kfs_write   v1_kfs_write
#define kfs_mkdir   v1_kfs_mkdir
#define kfs_unlink  v1_kfs_unlink
#define kfs_list    v1_kfs_list
#define kfs_info    v1_kfs_info
#define kfs_strerror v1_kfs_strerror
#define KFS_MAX_FILE ((12u + 1024u) * KFS_BLOCK)
#include "kfs_v1_impl.inc"

static uint8_t* vd; static uint64_t vn;
static int vrd(blk_dev_t* d, uint64_t l, uint32_t n, void* b) { (void)d; if (l + n > vn) return -1; memcpy(b, vd + l * 512, (size_t)n * 512); return 0; }
static int vwr(blk_dev_t* d, uint64_t l, uint32_t n, const void* b) { (void)d; if (l + n > vn) return -1; memcpy(vd + l * 512, b, (size_t)n * 512); return 0; }
static int vfl(blk_dev_t* d) { (void)d; return 0; }

int v1_make(uint8_t* disk, uint64_t sectors) {
    static blk_dev_t dv; static uint8_t data[3000000];
    vd = disk; vn = sectors;
    dv.sectors = sectors; dv.read = vrd; dv.write = vwr; dv.flush = vfl;
    if (v1_kfs_format(&dv, 16)) return -1;
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(21 * 31 + i * 7 + (i >> 8));
    if (v1_kfs_mkdir("/old") || v1_kfs_write("/old/file", data, sizeof data)) return -1;
    uint8_t h[5]; for (int i = 0; i < 5; i++) h[i] = (uint8_t)(22 * 31 + i * 7);
    return v1_kfs_write("/hello", h, 5);
}
