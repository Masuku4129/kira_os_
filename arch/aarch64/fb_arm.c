/* Экран ARM64 без virtio-gpu: ramfb (QEMU, «-device ramfb»: кадр в ОЗУ гостя, устройство настраивается через
   fw_cfg DMA) и simple-framebuffer из дерева устройств (кадровый буфер, поднятый прошивкой/U-Boot на настоящих
   платах). Всё берётся из дерева устройств: адрес fw_cfg, адрес/размер/шаг/формат кадра. */
#include <stdint.h>
#include "fdt.h"
#include "fb.h"
#include "virtio.h"          /* virtio_dma_alloc, MMIO_* */
#include "vmm.h"
#include "uart.h"
#include "platform.h"
#include "fb_arm.h"

static const void* dt;

static void clean(const void* p, uint64_t n) {             /* записанное - из кэша в память (устройство читает ОЗУ) */
    for (uint64_t a = (uint64_t)(uintptr_t)p & ~63ull; a < (uint64_t)(uintptr_t)p + n; a += 64) __asm__ volatile ("dc cvac, %0" : : "r"(a) : "memory");
    __asm__ volatile ("dsb sy" ::: "memory");
}
static uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }

/* ---------- ramfb через fw_cfg ---------- */
enum { FW_SIGNATURE = 0x0000, FW_ID = 0x0001, FW_FILE_DIR = 0x0019, DMA_ERROR = 1, DMA_SELECT = 8, DMA_WRITE = 16 };
typedef struct { uint32_t control, length; uint64_t address; } fwdma_t;
typedef struct { uint64_t addr; uint32_t fourcc, flags, width, height, stride; } __attribute__((packed)) ramfb_cfg_t;

static volatile uint8_t* fw;
static void fw_select(uint16_t key) { MMIO_W16(fw + 8, __builtin_bswap16(key)); }
static uint8_t fw_byte(void) { return MMIO_R8(fw); }
static uint32_t fw_be32(void) { uint32_t v = 0; for (int i = 0; i < 4; i++) v = v << 8 | fw_byte(); return v; }

static int fw_dma_write(uint16_t key, const void* data, uint32_t len) {
    uint64_t p; fwdma_t* d = (fwdma_t*)virtio_dma_alloc(sizeof *d, &p);
    if (!d) return -1;
    d->control = be32((uint32_t)key << 16 | DMA_SELECT | DMA_WRITE);
    d->length = be32(len);
    d->address = __builtin_bswap64((uint64_t)(uintptr_t)data);          /* тождественная карта: адрес = физический */
    clean(data, len); clean(d, sizeof *d);
    MMIO_W32(fw + 16, be32((uint32_t)(p >> 32)));
    MMIO_W32(fw + 20, be32((uint32_t)p));                               /* запись младшей половины запускает обмен */
    for (int i = 0; i < 1000000; i++) {
        __asm__ volatile ("dc civac, %0\n dsb sy" : : "r"(d) : "memory");
        uint32_t c = be32(*(volatile uint32_t*)&d->control);
        if (c & DMA_ERROR) return -2;
        if (!c) return 0;
    }
    return -3;
}

static int ramfb_probe(void) {
    int n = fdt_find_compatible(dt, -1, "qemu,fw-cfg-mmio");
    uint64_t base, sz;
    if (n < 0 || fdt_reg(dt, FDT_BOOT_MAX_SIZE, n, 0, &base, &sz) || sz < 24) return -1;
    fw = (volatile uint8_t*)vmm_map_mmio(base, sz, VMM_MMIO_UC);
    if (!fw) return -1;
    fw_select(FW_SIGNATURE);
    if (fw_byte() != 'Q' || fw_byte() != 'E' || fw_byte() != 'M' || fw_byte() != 'U') return -1;
    fw_select(FW_ID);
    uint32_t id = fw_byte();
    if (!(id & 2)) { uart_print("[KIRA FB] fw_cfg without DMA: ramfb cannot be configured.\n"); return -2; }
    fw_select(FW_FILE_DIR);
    uint32_t cnt = fw_be32(), sel = 0;
    for (uint32_t i = 0; i < cnt && !sel; i++) {
        uint32_t size = fw_be32(); (void)size;
        uint16_t s = (uint16_t)(fw_byte() << 8); s |= fw_byte();
        fw_byte(); fw_byte();
        char name[56]; for (int k = 0; k < 56; k++) name[k] = (char)fw_byte();
        const char want[] = "etc/ramfb";
        int eq = 1; for (unsigned k = 0; k < sizeof want; k++) if (name[k] != want[k]) { eq = 0; break; }
        if (eq) sel = s;
    }
    if (!sel) return -1;                                                /* fw_cfg есть, ramfb не подключён */
    /* ramfb не сообщает «родного» размера: режим выбирает гость. 1024x768 - как у virtio-gpu QEMU по умолчанию. */
    const uint32_t W = 1024, H = 768;
    uint64_t fbp; void* fbm = virtio_dma_alloc(W * H * 4, &fbp);
    uint64_t cp; ramfb_cfg_t* c = (ramfb_cfg_t*)virtio_dma_alloc(sizeof *c, &cp);
    if (!fbm || !c) { uart_print("[KIRA FB] no memory for the ramfb frame.\n"); return -3; }
    c->addr = __builtin_bswap64(fbp); c->fourcc = be32(0x34325258);      /* DRM 'XR24': 0x00RRGGBB, как у ядра */
    c->flags = 0; c->width = be32(W); c->height = be32(H); c->stride = be32(W * 4);
    int rc = fw_dma_write((uint16_t)sel, c, sizeof *c);
    if (rc) { uart_print("[KIRA FB] ramfb: fw_cfg DMA write failed.\n"); return -4; }
    if (fb_attach_linear(fbp, fbm, W, H, W * 4, 0) != 0) return -5;
    uart_print("[KIRA FB] ramfb: "); uart_print_dec(W); uart_print("x"); uart_print_dec(H);
    uart_print(" XRGB8888 frame in RAM, configured through fw_cfg DMA.\n");
    return 0;
}

/* ---------- simple-framebuffer ---------- */
static fdt_fb_t sfb;
static volatile uint32_t* sfb_dev;          /* кадр вне ОЗУ (память устройства): кадр ядра в ОЗУ копируется 32-битными записями */
static uint8_t* sfb_ram;                    /* кадр в ОЗУ: ядро пишет прямо в него, после кадра - очистка кэша */
static uint32_t* shadow;

static void sfb_flush(int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++) {
        if (sfb_ram) { clean(sfb_ram + (uint64_t)j * sfb.stride + (uint64_t)x * 4, (uint64_t)w * 4); continue; }
        const uint32_t* s = shadow + (uint64_t)j * sfb.width + x;
        volatile uint32_t* d = sfb_dev + ((uint64_t)j * sfb.stride) / 4 + x;
        for (int i = 0; i < w; i++) d[i] = s[i];
    }
    __asm__ volatile ("dsb sy" ::: "memory");
}

static int simplefb_probe(void) {
    int rc = fdt_simplefb(dt, FDT_BOOT_MAX_SIZE, &sfb);
    if (rc == -1) return -1;
    if (rc == -2) { uart_print("[KIRA FB] simple-framebuffer node is incomplete (reg/width/height/stride/format).\n"); return -2; }
    if (rc == -3) { uart_print("[KIRA FB] simple-framebuffer format "); uart_print(sfb.format); uart_print(" is not supported (need x8r8g8b8/a8r8g8b8).\n"); return -3; }
    void* front;
    if (arm_mem_in_ram(sfb.base, sfb.size)) { sfb_ram = (uint8_t*)(uintptr_t)sfb.base; front = sfb_ram; }
    else {
        sfb_dev = (volatile uint32_t*)vmm_map_mmio(sfb.base, sfb.size, VMM_MMIO_UC);
        uint64_t p; shadow = (uint32_t*)virtio_dma_alloc(sfb.width * sfb.height * 4, &p);
        if (!sfb_dev || !shadow) { uart_print("[KIRA FB] simple-framebuffer: cannot map the frame.\n"); return -4; }
        front = shadow;
    }
    if (fb_attach_linear(sfb.base, front, sfb.width, sfb.height, sfb_ram ? sfb.stride : sfb.width * 4, sfb_flush) != 0) return -5;
    uart_print("[KIRA FB] simple-framebuffer: "); uart_print_dec(sfb.width); uart_print("x"); uart_print_dec(sfb.height);
    uart_print(" "); uart_print(sfb.format); uart_print(sfb_ram ? ", frame in RAM (reserved)\n" : ", frame in device memory\n");
    return 0;
}

void arm_fb_init(const void* fdt) { dt = fdt; }

/* Кадр simple-framebuffer и /reserved-memory в ОЗУ - не отдавать распределителю страниц. */
void arm_fb_reserve(const void* fdt, void (*reserve)(uint64_t lo, uint64_t hi)) {
    fdt_fb_t f;
    if (fdt_simplefb(fdt, FDT_BOOT_MAX_SIZE, &f) == 0) reserve(f.base, f.base + f.size);
    int rm = fdt_find_path(fdt, "/reserved-memory"), depth = 0;
    if (rm < 0) return;
    for (int n = fdt_next_node(fdt, rm, &depth); n >= 0 && depth > 0; n = fdt_next_node(fdt, n, &depth)) {
        uint64_t a, s;
        if (depth == 1 && fdt_node_enabled(fdt, n))
            for (unsigned k = 0; fdt_reg(fdt, FDT_BOOT_MAX_SIZE, n, k, &a, &s) == 0; k++) if (s) reserve(a, a + s);
    }
}

int platform_fb_probe(void) {
    if (!dt) return -1;
    if (ramfb_probe() == 0) return 0;
    return simplefb_probe();
}
