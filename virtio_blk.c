#include "virtio.h"
#include "blk.h"
#include "uart.h"

/* virtio-blk (спецификация virtio 1.0, раздел 5.2). Данные идут через собственный DMA-буфер: драйвер не делает
   предположений о том, где лежит память вызывающего. */

#define BLK_F_SIZE_MAX (1ULL << 1)
#define BLK_F_SEG_MAX  (1ULL << 2)
#define BLK_F_RO       (1ULL << 5)
#define BLK_F_FLUSH    (1ULL << 9)

#define T_IN 0
#define T_OUT 1
#define T_FLUSH 4

#define BOUNCE_SECTORS 128                 /* 64 КБ за заявку */

typedef struct { uint32_t type, reserved; uint64_t sector; } req_hdr_t;

static virtio_dev_t vdev;
static virtq_t vq;
static blk_dev_t disk;
static int present, dead;
static uint32_t chunk_sectors;
static req_hdr_t* hdr; static uint64_t hdr_phys;
static volatile uint8_t* status_byte; static uint64_t status_phys;
static uint8_t* bounce; static uint64_t bounce_phys;
static uint8_t* blk_dma;
static uint64_t blk_dma_phys;
static uint32_t blk_dma_bytes;

static void blk_release_after_reset(void) {
    if (virtio_reset_device(&vdev) != 0) return; /* device may still DMA: retain all backing memory */
    if (blk_dma && virtio_dma_free(blk_dma, blk_dma_phys, blk_dma_bytes) == 0) {
        blk_dma = 0; blk_dma_phys = 0; blk_dma_bytes = 0;
        hdr = 0; hdr_phys = 0; status_byte = 0; status_phys = 0; bounce = 0; bounce_phys = 0;
    }
    if (vq.mem) (void)virtio_queue_release(&vq);
}

static void blk_abort(const char* message) {
    if (vdev.common) {
        virtio_fail(&vdev);
        blk_release_after_reset();
    }
    present = 0;
    uart_print(message);
}

static int request(uint32_t type, uint64_t sector, uint32_t sectors) {
    if (dead) return -5;
    hdr->type = type; hdr->reserved = 0; hdr->sector = sector;
    *status_byte = 0xFF;
    virtq_buf_t b[3];
    int n = 0;
    b[n++] = (virtq_buf_t){ hdr_phys, sizeof(req_hdr_t), 0 };
    if (type != T_FLUSH) b[n++] = (virtq_buf_t){ bounce_phys, sectors * BLK_SECTOR, type == T_IN };
    b[n++] = (virtq_buf_t){ status_phys, 1, 1 };
    if (virtio_dma_sync_for_device(blk_dma, blk_dma_phys, blk_dma_bytes) != 0) {
        dead = 1; virtio_fail(&vdev); blk_release_after_reset(); present = 0; return -6;
    }
    if (virtq_run(&vq, b, n) != 0) {
        dead = 1;
        virtio_fail(&vdev);
        blk_release_after_reset();
        present = 0;
        uart_print("[KIRA VIRTIO] blk request timeout: device disabled.\n");
        return -4;
    }
    if (virtio_dma_sync_for_cpu(blk_dma, blk_dma_phys, blk_dma_bytes) != 0) {
        dead = 1; virtio_fail(&vdev); blk_release_after_reset(); present = 0; return -6;
    }
    return *status_byte == 0 ? 0 : -3;
}

static int vblk_read(blk_dev_t* d, uint64_t lba, uint32_t count, void* buf) {
    if (lba + count > d->sectors || lba + count < lba) return -1;
    uint8_t* p = (uint8_t*)buf;
    while (count) {
        uint32_t n = count < chunk_sectors ? count : chunk_sectors;
        int r = request(T_IN, lba, n);
        if (r) return r;
        for (uint32_t i = 0; i < n * BLK_SECTOR; i++) p[i] = bounce[i];
        p += n * BLK_SECTOR; lba += n; count -= n;
    }
    return 0;
}

static int vblk_write(blk_dev_t* d, uint64_t lba, uint32_t count, const void* buf) {
    if (lba + count > d->sectors || lba + count < lba) return -1;
    if (vdev.features & BLK_F_RO) return -6;
    const uint8_t* p = (const uint8_t*)buf;
    while (count) {
        uint32_t n = count < chunk_sectors ? count : chunk_sectors;
        for (uint32_t i = 0; i < n * BLK_SECTOR; i++) bounce[i] = p[i];
        int r = request(T_OUT, lba, n);
        if (r) return r;
        p += n * BLK_SECTOR; lba += n; count -= n;
    }
    return 0;
}

static int vblk_flush(blk_dev_t* d) {
    (void)d;
    if (!(vdev.features & BLK_F_FLUSH)) return 0;          /* устройство без кэша записи: запись уже долговечна */
    return request(T_FLUSH, 0, 0);
}

#ifndef KIRA_HOST_TEST
#include "msi.h"
static irqwait_t vblk_iw = IRQWAIT_INIT;
static void vblk_irq(void* ctx) { (void)ctx; irqwait_fire(&vblk_iw); }
uint64_t virtio_blk_irq_wakeups(void) { return vblk_iw.wakeups; }
#endif

void virtio_blk_shutdown(void) {
    if (!vdev.common) {
        present = 0;
        dead = 1;
        return;
    }
    virtio_fail(&vdev);
    blk_release_after_reset();
    present = 0;
    dead = 1;
}

blk_dev_t* virtio_blk_probe(void) {
    if (present) return &disk;
    if (vq.mem || blk_dma) return 0; /* a previous probe retained live DMA ownership */
    present = 0; dead = 0;
    const pci_dev_t* pd = pci_find(VIRTIO_VENDOR, 0x1042);     /* virtio-blk, современный */
    if (!pd) pd = pci_find(VIRTIO_VENDOR, 0x1001);              /* «переходный»: тот же прибор, но с capabilities 1.0 */
    if (!pd) return 0;
    if (virtio_attach(&vdev, pd) != 0) { uart_print("[KIRA VIRTIO] blk: no virtio 1.0 capabilities (legacy-only device unsupported).\n"); return 0; }
    if (!vdev.devcfg) { blk_abort("[KIRA VIRTIO] blk: no device config.\n"); return 0; }
    if (virtio_begin(&vdev, BLK_F_SIZE_MAX | BLK_F_SEG_MAX | BLK_F_RO | BLK_F_FLUSH) != 0) {
        blk_abort("[KIRA VIRTIO] blk: feature negotiation failed.\n"); return 0;
    }
    int irq = 0;
#ifndef KIRA_HOST_TEST
    {   /* прерывание очереди сообщением MSI-X: поток спит до ответа диска, а не проверяет его каждый тик */
        uint64_t ma; uint32_t md;
        if (msi_alloc(pd, vblk_irq, 0, &ma, &md) == 0 && pci_msix_setup(pd, 0, ma, md) == 0) { vdev.msix_vec = 0; irq = 1; }
    }
#endif
    if (virtio_queue_setup(&vdev, &vq, 0, 64) != 0) {
        blk_abort("[KIRA VIRTIO] blk: queue setup failed.\n"); return 0;
    }
#ifndef KIRA_HOST_TEST
    if (irq && MMIO_R16(vdev.common + 26) == 0) vq.iw = &vblk_iw; else irq = 0;
#endif

    uint64_t dma_phys;
    uint32_t dma_bytes = 4096 + BOUNCE_SECTORS * BLK_SECTOR;
    uint8_t* dma = (uint8_t*)virtio_dma_alloc(dma_bytes, &dma_phys);
    if (!dma) { blk_abort("[KIRA VIRTIO] blk: no DMA memory.\n"); return 0; }
    blk_dma = dma; blk_dma_phys = dma_phys; blk_dma_bytes = dma_bytes;
    hdr = (req_hdr_t*)dma; hdr_phys = dma_phys;
    status_byte = dma + 64; status_phys = dma_phys + 64;
    bounce = dma + 4096; bounce_phys = dma_phys + 4096;

    chunk_sectors = BOUNCE_SECTORS;
    if (vdev.features & BLK_F_SIZE_MAX) {
        uint32_t smax = MMIO_R32(vdev.devcfg + 8);
        if (smax >= BLK_SECTOR && smax / BLK_SECTOR < chunk_sectors) chunk_sectors = smax / BLK_SECTOR;
        if (smax && smax < BLK_SECTOR) {
            blk_abort("[KIRA VIRTIO] blk: size_max below one sector.\n"); return 0;
        }
    }
    virtio_driver_ok(&vdev);

    uint64_t cap = MMIO_R32(vdev.devcfg + 0) | ((uint64_t)MMIO_R32(vdev.devcfg + 4) << 32);
    if (cap < 16) { blk_abort("[KIRA VIRTIO] blk: disk too small.\n"); return 0; }
    disk.sectors = cap;
    disk.read = vblk_read; disk.write = vblk_write; disk.flush = vblk_flush; disk.priv = 0;
    const char* nm = "virtio-blk"; unsigned i = 0;
    while (nm[i] && i + 1 < sizeof disk.name) { disk.name[i] = nm[i]; i++; }
    disk.name[i] = 0;
    present = 1;
    uart_print("[KIRA VIRTIO] Disk virtio-blk, "); uart_print_dec(cap / 2048); uart_print(" MB (PCI ");
    uart_print_dec(pd->bus); uart_print(":"); uart_print_dec(pd->dev); uart_print(".");  uart_print_dec(pd->fn);
    uart_print(irq ? ", virtio 1.0, completion by MSI-X interrupt" : ", virtio 1.0, polling");
    if (vdev.features & BLK_F_RO) uart_print(", READ-ONLY");
    uart_print(").\n");
    return &disk;
}
