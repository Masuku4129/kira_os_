#include "usbmsc_fs.h"
#include "usb.h"
#include "part.h"
#include "fat.h"
#include "vfs.h"

/* Тома FAT для накопителей USB (ур.3): драйвер usbmsc.c (ур.2) блочное устройство создаёт, а раздел находит, монтирует и снимает
   эта сторона через msc_fs_ops_t (usb.h). Подключается в wire_init. */

static fat_vol_t vols[USB_MAX_MSC];          /* том накопителя с номером slot */

static int msc_fs_mount(int slot, blk_dev_t* blk, const char* vendor, const char* product) {
    static uint8_t s0[512];
    if (blk->read(blk, 0, 1, s0) != 0) { usb_log("[KIRA USB] storage: cannot read sector 0\n"); return -1; }
    static part_t pt[PART_MAX];                                        /* не на стеке потока usb */
    int np = part_scan(blk, s0, pt, PART_MAX);
    fat_vol_t* fat = &vols[slot];
    for (int i = 0; i < np; i++) {
        if (fat_mount(fat, blk, pt[i].start, pt[i].count) != 0) continue;
        char desc[48]; int k = 0;
        for (const char* p = vendor; *p && k < 20; p++) desc[k++] = *p;
        if (k) desc[k++] = ' ';
        for (const char* p = product; *p && k < 46; p++) desc[k++] = *p;
        desc[k] = 0;
        int vol = vfs_mount_fat_ro(fat, desc);
        if (vol < 0) { usb_log("[KIRA USB] storage: no free mount point (4 volumes max)\n"); return vol; }
        usb_log("[KIRA USB] storage: FAT"); usb_log_dec((uint64_t)fat->type);
        if (fat->label[0]) { usb_log(" '"); usb_log(fat->label); usb_log("'"); }
        usb_log(" mounted at /usb"); usb_log_dec((uint64_t)vol); usb_log("\n");
        return vol;
    }
    if (np == 0 && s0[3] == 'E' && s0[4] == 'X' && s0[5] == 'F' && s0[6] == 'A' && s0[7] == 'T') usb_log("[KIRA USB] storage: exFAT is not supported yet (format the stick as FAT32)\n");
    else if (np == 0 && s0[3] == 'N' && s0[4] == 'T' && s0[5] == 'F' && s0[6] == 'S') usb_log("[KIRA USB] storage: NTFS is not supported yet (format the stick as FAT32)\n");
    else usb_log("[KIRA USB] storage: no FAT volume found (exFAT/NTFS or unformatted)\n");
    return -1;
}

static int msc_fs_unmount_try(int vol) { return vfs_unmount_try(vol); }

const msc_fs_ops_t usbmsc_fs_ops = { msc_fs_mount, msc_fs_unmount_try };
