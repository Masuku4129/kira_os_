/* virtio-rng (энтропия от хозяина машины: QEMU «-device virtio-rng-pci», облака). Одна очередь: драйвер отдаёт
   буфер, устройство заполняет его случайными байтами. Читается при засевании генератора ядра. */
#include "virtio.h"
#include "rng.h"
#ifndef KIRA_HOST_TEST
#include "devpm.h"
#endif

static virtio_dev_t dev;
static virtq_t q;
static int st;                       /* 0 - не искали, 1 - работает, -1 - нет */
static uint8_t* buf; static uint64_t buf_p;
static volatile int asleep;          /* сон S3: устройство сброшено */
#ifndef KIRA_HOST_TEST
static int vrng_suspend(void* ctx) { (void)ctx; if (st == 1) { asleep = 1; virtio_stop(&dev); } return 0; }
static int vrng_resume(void* ctx) {
    (void)ctx;
    if (st != 1) return 0;
    if (virtio_resume_begin(&dev) || virtio_queue_resume(&dev, &q)) { virtio_fail(&dev); st = -1; asleep = 0; return -1; }
    virtio_driver_ok(&dev);
    asleep = 0;
    return 0;
}
#endif

int virtio_rng_read(void* out, uint32_t n) {
    if (!st) {
        st = -1;
        const pci_dev_t* p = pci_find(VIRTIO_VENDOR, 0x1044);
        if (!p) p = pci_find(VIRTIO_VENDOR, 0x1005);
        if (!p) return -1;
        int iso = virtio_iso_open(p);
        if (virtio_attach(&dev, p)) return -1;
        if (iso) iso = virtio_iso_confirm(&dev, "virtio-rng", 0, 1);        /* устройство само пишет энтропию в выданный буфер */
        if (virtio_begin(&dev, VIRTIO_F_ACCESS_PLATFORM) || virtio_queue_setup(&dev, &q, 0, 8)) return -1;
        buf = (uint8_t*)virtio_dma_alloc(64, &buf_p);
        if (!buf || (iso && virtio_dma_grant(&dev, buf_p, 64))) { virtio_fail(&dev); return -1; }
        virtio_driver_ok(&dev);
        st = 1;
#ifndef KIRA_HOST_TEST
        (void)devpm_register("virtio-rng", vrng_suspend, vrng_resume, 0);
#endif
    }
    if (st < 0 || asleep) return -1;
    if (n > 64) n = 64;
    virtq_buf_t b = { buf_p, n, 1 };
    if (virtq_run(&q, &b, 1) != 0) return -1;
    for (uint32_t i = 0; i < n; i++) ((uint8_t*)out)[i] = buf[i];
    for (uint32_t i = 0; i < n; i++) buf[i] = 0;
    return (int)n;
}
