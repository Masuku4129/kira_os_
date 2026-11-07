/* virtio-gpu (2D): экран через PCI на любой архитектуре - QEMU virt на ARM64 (Limbo ARM, телефоны), а на x86-64
   там, где нет кадрового буфера от загрузчика. Ядро держит изображение в своей памяти (ресурс 2D с подложкой
   из страниц ОЗУ), после каждого кадра сообщает устройству изменённый прямоугольник: перенос и показ.
   Обмен - синхронный, по одной команде (очередь управления), как у остальных драйверов virtio. */
#include "virtio.h"
#include "virtio_gpu.h"
#include "fb.h"
#include "uart.h"
#include "spinlock.h"

#define VIRTIO_GPU_DEVICE 0x1050           /* 0x1040 + 16 */

enum {
    CMD_GET_DISPLAY_INFO = 0x0100, CMD_RESOURCE_CREATE_2D, CMD_RESOURCE_UNREF, CMD_SET_SCANOUT, CMD_RESOURCE_FLUSH,
    CMD_TRANSFER_TO_HOST_2D, CMD_RESOURCE_ATTACH_BACKING,
    RESP_OK_NODATA = 0x1100, RESP_OK_DISPLAY_INFO = 0x1101
};
#define FORMAT_B8G8R8X8 2                   /* байты B, G, R, X - то же, что 0x00RRGGBB в памяти little-endian */

typedef struct { uint32_t type, flags; uint64_t fence; uint32_t ctx, pad; } hdr_t;
typedef struct { uint32_t x, y, w, h; } rect_t;
typedef struct { hdr_t h; struct { rect_t r; uint32_t enabled, flags; } mode[16]; } disp_info_t;
typedef struct { hdr_t h; uint32_t res, format, width, height; } create_t;
typedef struct { hdr_t h; uint32_t res, n; uint64_t addr; uint32_t len, pad; } attach_t;
typedef struct { hdr_t h; rect_t r; uint32_t scanout, res; } scanout_t;
typedef struct { hdr_t h; rect_t r; uint64_t off; uint32_t res, pad; } transfer_t;
typedef struct { hdr_t h; rect_t r; uint32_t res, pad; } flush_t;

static virtio_dev_t dev;
static virtq_t ctrl;
static uint8_t* cmd;  static uint64_t cmd_phys;    /* одна страница: команда */
static uint8_t* resp; static uint64_t resp_phys;   /* и ответ (сведения об экранах - 408 байт) */
static uint8_t* fbm; static uint64_t fb_phys; static uint32_t fb_bytes;
static uint32_t W, H;
static spinlock_t lk = SPINLOCK_INIT;
static int up;

static uint32_t send(uint32_t len, uint32_t rlen) {
    virtq_buf_t b[2] = { { cmd_phys, len, 0 }, { resp_phys, rlen, 1 } };
    ((hdr_t*)resp)->type = 0;
    if (virtio_dma_sync_for_device(cmd, cmd_phys, len) != 0 ||
        virtio_dma_sync_for_device(resp, resp_phys, rlen) != 0 ||
        virtq_run(&ctrl, b, 2) != 0 ||
        virtio_dma_sync_for_cpu(resp, resp_phys, rlen) != 0) return 0;
    return ((hdr_t*)resp)->type;
}

static void gpu_abort(void) {
    up = 0;
    if (dev.common) {
        virtio_fail(&dev);
        if (virtio_reset_device(&dev) != 0) return; /* never free memory while GPU may still DMA */
    }
    if (fbm && virtio_dma_free(fbm, fb_phys, fb_bytes) == 0) {
        fbm = 0; fb_phys = 0; fb_bytes = 0;
    }
    if (resp && virtio_dma_free(resp, resp_phys, 4096) == 0) {
        resp = 0; resp_phys = 0;
    }
    if (cmd && virtio_dma_free(cmd, cmd_phys, 4096) == 0) {
        cmd = 0; cmd_phys = 0;
    }
    if (ctrl.mem) (void)virtio_queue_release(&ctrl);
    if (!fbm && !resp && !cmd && !ctrl.mem) {
        dev = (virtio_dev_t){0};
        ctrl = (virtq_t){0};
    }
}

static void zero(void* p, unsigned n) { uint8_t* b = (uint8_t*)p; while (n--) *b++ = 0; }

/* Изменённая область кадра - устройству: перенос из подложки и показ. Вызывается после сборки кадра. */
static void gpu_flush(int x, int y, int w, int h) {
    if (!up || w <= 0 || h <= 0) return;
    uint64_t f = spin_lock_irqsave(&lk);
    if (virtio_dma_sync_for_device(fbm, fb_phys, fb_bytes) != 0) {
        up = 0;
        uart_print("[KIRA GPU] framebuffer DMA synchronization failed; screen frozen.\\n");
        spin_unlock_irqrestore(&lk, f);
        virtio_fail(&dev);
        (void)virtio_reset_device(&dev);
        return;
    }
    transfer_t* t = (transfer_t*)cmd; zero(t, sizeof *t);
    t->h.type = CMD_TRANSFER_TO_HOST_2D; t->r = (rect_t){ (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h };
    t->off = ((uint64_t)y * W + (uint64_t)x) * 4; t->res = 1;
    uint32_t r = send(sizeof *t, sizeof(hdr_t));
    flush_t* fl = (flush_t*)cmd; zero(fl, sizeof *fl);
    fl->h.type = CMD_RESOURCE_FLUSH; fl->r = (rect_t){ (uint32_t)x, (uint32_t)y, (uint32_t)w, (uint32_t)h }; fl->res = 1;
    if (r == RESP_OK_NODATA) r = send(sizeof *fl, sizeof(hdr_t));
    spin_unlock_irqrestore(&lk, f);
    if (r != RESP_OK_NODATA) {
        up = 0;
        uart_print("[KIRA GPU] virtio-gpu stopped answering: the screen is frozen, the system keeps running.\n");
        virtio_fail(&dev);
        (void)virtio_reset_device(&dev); /* retain attached framebuffer memory */
    }
}

int virtio_gpu_probe(void) {
    if (dev.common || ctrl.mem || cmd || resp || fbm) return -8; /* previous device may still own DMA memory */
    const pci_dev_t* p = pci_find(VIRTIO_VENDOR, VIRTIO_GPU_DEVICE);
    if (!p) return -1;
    if (virtio_attach(&dev, p) || !dev.devcfg || virtio_begin(&dev, 0) ||
        virtio_queue_setup(&dev, &ctrl, 0, 64)) {
        gpu_abort();
        uart_print("[KIRA GPU] virtio-gpu found but did not start.\n");
        return -2;
    }
    virtio_driver_ok(&dev);
    cmd = (uint8_t*)virtio_dma_alloc(4096, &cmd_phys);
    resp = (uint8_t*)virtio_dma_alloc(4096, &resp_phys);
    if (!cmd || !resp) { gpu_abort(); return -3; }

    disp_info_t* di = (disp_info_t*)resp;
    zero(cmd, sizeof(hdr_t)); ((hdr_t*)cmd)->type = CMD_GET_DISPLAY_INFO;
    if (send(sizeof(hdr_t), sizeof *di) != RESP_OK_DISPLAY_INFO) { gpu_abort(); return -4; }
    W = di->mode[0].r.w; H = di->mode[0].r.h;
    if (!di->mode[0].enabled || W < 320 || H < 200) { W = 1024; H = 768; }   /* экран не задан хозяином - обычный */
    if (W > 4096) W = 4096;
    if (H > 4096) H = 4096;
    W &= ~1u;

    fb_bytes = W * H * 4;
    fbm = (uint8_t*)virtio_dma_alloc(fb_bytes, &fb_phys);   /* подложка: непрерывная, видна устройству */
    if (!fbm) { gpu_abort(); return -5; }

    create_t* c = (create_t*)cmd; zero(c, sizeof *c);
    c->h.type = CMD_RESOURCE_CREATE_2D; c->res = 1; c->format = FORMAT_B8G8R8X8; c->width = W; c->height = H;
    attach_t* a; scanout_t* s;
    int ok = send(sizeof *c, sizeof(hdr_t)) == RESP_OK_NODATA;
    if (ok) {
        a = (attach_t*)cmd; zero(a, sizeof *a);
        a->h.type = CMD_RESOURCE_ATTACH_BACKING; a->res = 1; a->n = 1; a->addr = fb_phys; a->len = W * H * 4;
        ok = send(sizeof *a, sizeof(hdr_t)) == RESP_OK_NODATA;
    }
    if (ok) {
        s = (scanout_t*)cmd; zero(s, sizeof *s);
        s->h.type = CMD_SET_SCANOUT; s->r = (rect_t){ 0, 0, W, H }; s->scanout = 0; s->res = 1;
        ok = send(sizeof *s, sizeof(hdr_t)) == RESP_OK_NODATA;
    }
    if (!ok) { uart_print("[KIRA GPU] virtio-gpu refused the 2D resource.\n"); gpu_abort(); return -6; }
    up = 1;
    if (fb_attach_linear(fb_phys, fbm, W, H, W * 4, gpu_flush) != 0) { gpu_abort(); return -7; }
    uart_print("[KIRA GPU] virtio-gpu: "); uart_print_dec(W); uart_print("x"); uart_print_dec(H);
    uart_print(" 2D scanout, frames are sent to the host after each composed frame.\n");
    return 0;
}
