#include "ramdisk.h"
#include "mbinfo.h"
#include "kfs.h"
#include "uart.h"
#include <stdint.h>

#define MB2_TAG_MODULE 3u
#define RAMDISK_MIN_SECTORS 64u

static unsigned long boot_info;

void ramdisk_set_boot_info(unsigned long mbi) {
    boot_info = mbi;
}

static int module_is_root(const uint8_t* cmd, uint32_t len) {
    static const char name[] = "kira-root";
    if (!cmd || len < sizeof name) return 0;
    for (unsigned i = 0; i < sizeof name - 1; i++) if (cmd[i] != (uint8_t)name[i]) return 0;
    return cmd[sizeof name - 1] == 0;
}

static int ram_read(blk_dev_t* dev, uint64_t lba, uint32_t count, void* out) {
    if (!dev || !out || lba > dev->sectors || count > dev->sectors - lba) return -1;
    const uint8_t* src = (const uint8_t*)dev->priv + lba * BLK_SECTOR;
    uint8_t* dst = (uint8_t*)out;
    uint64_t bytes = (uint64_t)count * BLK_SECTOR;
    for (uint64_t i = 0; i < bytes; i++) dst[i] = src[i];
    return 0;
}

static int ram_write(blk_dev_t* dev, uint64_t lba, uint32_t count, const void* in) {
    if (!dev || !in || lba > dev->sectors || count > dev->sectors - lba) return -1;
    uint8_t* dst = (uint8_t*)dev->priv + lba * BLK_SECTOR;
    const uint8_t* src = (const uint8_t*)in;
    uint64_t bytes = (uint64_t)count * BLK_SECTOR;
    for (uint64_t i = 0; i < bytes; i++) dst[i] = src[i];
    return 0;
}

static int ram_flush(blk_dev_t* dev) {
    return dev && dev->priv ? 0 : -1;
}

blk_dev_t* ramdisk_probe(void) {
    const uint32_t* tag = (const uint32_t*)mbinfo_find_tag(boot_info, MB2_TAG_MODULE);
    if (!tag) {
        uart_print("[KIRA RAMDISK] Multiboot2 module tag is missing.\\n");
        return 0;
    }
    if (tag[1] < 17) {
        uart_print("[KIRA RAMDISK] module tag is too short: "); uart_print_dec(tag[1]); uart_print("\\n");
        return 0;
    }
    uint32_t start = tag[2], end = tag[3];
    const uint8_t* cmd = (const uint8_t*)(tag + 4);
    uint32_t cmd_len = tag[1] - 16;
    if (!module_is_root(cmd, cmd_len)) {
        uart_print("[KIRA RAMDISK] module cmdline mismatch: ");
        for (uint32_t i = 0; i < cmd_len && cmd[i]; i++) uart_putc((char)cmd[i]);
        uart_print("\\n");
        return 0;
    }
    if (end <= start) {
        uart_print("[KIRA RAMDISK] invalid module bounds.\\n");
        return 0;
    }
    uint32_t bytes = end - start;
    if ((bytes % BLK_SECTOR) != 0 || bytes / BLK_SECTOR < RAMDISK_MIN_SECTORS) {
        uart_print("[KIRA RAMDISK] invalid module size: "); uart_print_dec(bytes); uart_print(" bytes\\n");
        return 0;
    }

    static blk_dev_t dev;
    static const char name[] = "kira-iso-ram";
    unsigned i = 0;
    for (; i + 1 < sizeof dev.name && i < sizeof name - 1; i++) dev.name[i] = name[i];
    dev.name[i] = 0;
    dev.sectors = bytes / BLK_SECTOR;
    dev.read = ram_read;
    dev.write = ram_write;
    dev.flush = ram_flush;
    dev.priv = (void*)(uintptr_t)start;
    int detected = kfs_detect(&dev, KFS_BASE_LBA);
    uart_print("[KIRA RAMDISK] module bytes="); uart_print_dec(bytes);
    uart_print(" start="); uart_print_hex(start);
    uart_print(" KiraFS detect="); uart_print_dec((uint64_t)(uint32_t)detected);
    uart_print("\n");
    if (detected != 0) {
        uint8_t probe[BLK_SECTOR];
        if (ram_read(&dev, KFS_BASE_LBA, 1, probe) == 0) {
            uart_print("[KIRA RAMDISK] superblock prefix=");
            for (unsigned k = 0; k < 16; k++) { uart_print_hex(probe[k]); uart_print(" "); }
            uart_print("\n");
        }
    }
    return &dev;
}
