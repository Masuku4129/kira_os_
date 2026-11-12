/* virtio-rng (энтропия от хозяина машины: QEMU «-device virtio-rng-pci», облака). Одна очередь: драйвер отдаёт
   буфер, устройство заполняет его случайными байтами. Читается при засевании генератора ядра. */
#include "virtio.h"
#include "rng.h"

static virtio_dev_t dev;
static virtq_t q;
static int st;                       /* 0 - не искали, 1 - работает, -1 - нет */
static uint8_t* buf; static uint64_t buf_p;

static void rng_abort(void) {
    if (dev.common) {
        virtio_fail(&dev);
        if (virtio_reset_device(&dev) != 0) { st = -1; return; }
    }
    if (buf) {
        if (virtio_dma_free(buf, buf_p, 64) != 0) { st = -1; return; }
        buf = 0; buf_p = 0;
    }
    if (q.mem && virtio_queue_release(&q) != 0) { st = -1; return; }
    dev = (virtio_dev_t){0};
    q = (virtq_t){0};
    st = -1;
}

int virtio_rng_read(void* out, uint32_t n) {
    if (!st) {
        st = -1;
        const pci_dev_t* p = pci_find(VIRTIO_VENDOR, 0x1044);
        if (!p) p = pci_find(VIRTIO_VENDOR, 0x1005);
        if (!p) return -1;
        if (virtio_attach(&dev, p) || virtio_begin(&dev, 0) ||
            virtio_queue_setup(&dev, &q, 0, 8)) { rng_abort(); return -1; }
        buf = (uint8_t*)virtio_dma_alloc(64, &buf_p);
        if (!buf) { rng_abort(); return -1; }
        if (virtio_driver_ok(&dev) != 0) { rng_abort(); return -1; }
        st = 1;
    }
    if (st < 0) return -1;
    if (n > 64) n = 64;
    virtq_buf_t b = { buf_p, n, 1 };
    if (virtio_dma_sync_for_device(buf, buf_p, n) != 0 ||
        virtq_run(&q, &b, 1) != 0 ||
        virtio_dma_sync_for_cpu(buf, buf_p, n) != 0) {
        rng_abort();
        return -1;
    }
    for (uint32_t i = 0; i < n; i++) ((uint8_t*)out)[i] = buf[i];
    for (uint32_t i = 0; i < n; i++) buf[i] = 0;
    return (int)n;
}
