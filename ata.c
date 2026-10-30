#include "blk.h"
#include "uart.h"

/* ATA PIO, первичный канал, ведущий диск, LBA28. Опрос без прерываний (nIEN).
   Это самый простой из драйверов, который работает в любом эмуляторе и на старом железе;
   всё остальное ядро видит только blk_dev_t, так что замена драйвера (virtio-blk, NVMe)
   не затрагивает верхние слои. */

#define ATA_DATA    0x1F0
#define ATA_ERR     0x1F1
#define ATA_COUNT   0x1F2
#define ATA_LBA0    0x1F3
#define ATA_LBA1    0x1F4
#define ATA_LBA2    0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_STATUS  0x1F7
#define ATA_CMD     0x1F7
#define ATA_CTRL    0x3F6

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF  0x20
#define ST_RDY 0x40
#define ST_BSY 0x80

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void insw(uint16_t p, void* b, uint32_t n) { __asm__ volatile ("rep insw" : "+D"(b), "+c"(n) : "d"(p) : "memory"); }
static inline void outsw(uint16_t p, const void* b, uint32_t n) { __asm__ volatile ("rep outsw" : "+S"(b), "+c"(n) : "d"(p) : "memory"); }

static blk_dev_t disk;

static void delay400ns(void) { for (int i = 0; i < 4; i++) (void)inb(ATA_CTRL); }

/* Ждёт, пока BSY сбросится. Возвращает статус или -1 по таймауту. */
static int wait_not_busy(void) {
    for (uint32_t i = 0; i < 2000000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (!(s & ST_BSY)) return s;
    }
    return -1;
}

/* Ждёт DRQ. 0 - готово, -1 - таймаут, -2 - ошибка устройства. */
static int wait_drq(void) {
    for (uint32_t i = 0; i < 2000000; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & ST_BSY) continue;
        if (s & (ST_ERR | ST_DF)) return -2;
        if (s & ST_DRQ) return 0;
    }
    return -1;
}

static void select_lba(uint64_t lba) {
    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    delay400ns();
    outb(ATA_COUNT, 1);
    outb(ATA_LBA0, (uint8_t)lba);
    outb(ATA_LBA1, (uint8_t)(lba >> 8));
    outb(ATA_LBA2, (uint8_t)(lba >> 16));
}

static int ata_read(blk_dev_t* d, uint64_t lba, uint32_t count, void* buf) {
    if (lba + count > d->sectors || lba + count < lba) return -1;
    uint8_t* p = (uint8_t*)buf;
    for (uint32_t i = 0; i < count; i++, lba++, p += BLK_SECTOR) {
        if (wait_not_busy() < 0) return -2;
        select_lba(lba);
        outb(ATA_CMD, 0x20);
        if (wait_drq() != 0) return -3;
        insw(ATA_DATA, p, 256);
    }
    return 0;
}

static int ata_write(blk_dev_t* d, uint64_t lba, uint32_t count, const void* buf) {
    if (lba + count > d->sectors || lba + count < lba) return -1;
    const uint8_t* p = (const uint8_t*)buf;
    for (uint32_t i = 0; i < count; i++, lba++, p += BLK_SECTOR) {
        if (wait_not_busy() < 0) return -2;
        select_lba(lba);
        outb(ATA_CMD, 0x30);
        if (wait_drq() != 0) return -3;
        outsw(ATA_DATA, p, 256);
        int s = wait_not_busy();
        if (s < 0 || (s & (ST_ERR | ST_DF))) return -4;
    }
    return 0;
}

static int ata_flush(blk_dev_t* d) {
    (void)d;
    if (wait_not_busy() < 0) return -2;
    outb(ATA_DRIVE, 0xE0);
    delay400ns();
    outb(ATA_CMD, 0xE7); /* FLUSH CACHE */
    int s = wait_not_busy();
    if (s < 0 || (s & (ST_ERR | ST_DF))) return -4;
    return 0;
}

blk_dev_t* ata_probe(void) {
    outb(ATA_CTRL, 0x02);              /* nIEN: без прерываний, только опрос */
    outb(ATA_DRIVE, 0xA0);             /* ведущий */
    delay400ns();
    uint8_t st = inb(ATA_STATUS);
    if (st == 0xFF || st == 0x00) {
        uart_print("[KIRA ATA] No disk on the primary IDE channel (floating bus): persistence disabled.\n");
        return 0;
    }
    if (wait_not_busy() < 0) { uart_print("[KIRA ATA] Primary channel stuck busy: persistence disabled.\n"); return 0; }
    outb(ATA_COUNT, 0); outb(ATA_LBA0, 0); outb(ATA_LBA1, 0); outb(ATA_LBA2, 0);
    outb(ATA_CMD, 0xEC);               /* IDENTIFY DEVICE */
    delay400ns();
    st = inb(ATA_STATUS);
    if (st == 0) { uart_print("[KIRA ATA] Drive 0 absent.\n"); return 0; }
    if (wait_not_busy() < 0) { uart_print("[KIRA ATA] IDENTIFY timeout.\n"); return 0; }
    if (inb(ATA_LBA1) || inb(ATA_LBA2)) { uart_print("[KIRA ATA] Primary master is not an ATA disk (ATAPI/SATA signature).\n"); return 0; }
    if (wait_drq() != 0) { uart_print("[KIRA ATA] IDENTIFY failed.\n"); return 0; }

    uint16_t id[256];
    insw(ATA_DATA, id, 256);
    if (!(id[49] & (1 << 9))) { uart_print("[KIRA ATA] Disk has no LBA support: unsupported.\n"); return 0; }
    uint32_t lba28 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    if (lba28 < 16) { uart_print("[KIRA ATA] Disk too small.\n"); return 0; }

    disk.sectors = lba28;
    disk.read = ata_read; disk.write = ata_write; disk.flush = ata_flush; disk.priv = 0;
    unsigned n = 0;                    /* модель: слова 27..46, байты в слове переставлены */
    for (int w = 27; w < 47 && n + 2 < sizeof(disk.name); w++) {
        char a = (char)(id[w] >> 8), b = (char)(id[w] & 0xFF);
        if (a) disk.name[n++] = a;
        if (b) disk.name[n++] = b;
    }
    while (n > 0 && disk.name[n - 1] == ' ') n--;
    disk.name[n] = 0;

    uart_print("[KIRA ATA] Disk '"); uart_print(disk.name);
    uart_print("', "); uart_print_dec(disk.sectors / 2048); uart_print(" MB (PIO, LBA28).\n");
    return &disk;
}
