#include "fb.h"
#include "pmm.h"
#include "vmm.h"
#include "uart.h"
#include "fbpix.h"
#include "spinlock.h"

#define MB2_TAG_END 0
#define MB2_TAG_FRAMEBUFFER 8
#define ONE_GB (1ULL << 30)

typedef struct {
    uint32_t type, size;
    uint64_t addr;
    uint32_t pitch, width, height;
    uint8_t bpp, fb_type;
    uint16_t reserved;
    uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
} __attribute__((packed)) mb2_fb_tag_t;

static fb_info_t info;
static int ready;
static surface_t back;
static uint8_t* front;     // видеопамять (виртуальный адрес)
static fbpix_fmt_t fmt;    // формат пикселя видеопамяти
static int native;         // 32 бита 0x00RRGGBB: копирование без перевода
static void (*flush_hook)(int, int, int, int);
static spinlock_t dlk = SPINLOCK_INIT;
static int dx0 = 1 << 30, dy0 = 1 << 30, dx1, dy1;   /* охват изменений кадра для flush_hook (полосы - с разных ядер) */

int fb_available(void) { return ready; }
const fb_info_t* fb_get_info(void) { return &info; }
surface_t* fb_surface(void) { return ready ? &back : 0; }

int fb_init(unsigned long mbi) {
    if (mbi == 0 || (mbi & 7) || mbi >= ONE_GB) return -1;
    uint64_t total = *(const uint32_t*)mbi;
    if (total < 16 || mbi + total > ONE_GB) return -1;

    const mb2_fb_tag_t* tag = 0;
    uint32_t seen_mask = 0;
    const uint8_t* p = (const uint8_t*)(mbi + 8);
    const uint8_t* end = (const uint8_t*)(mbi + total);
    while (p + 8 <= end) {
        const uint32_t* h = (const uint32_t*)p;
        if (h[0] == MB2_TAG_END) break;
        if (h[1] < 8 || p + h[1] > end) break;
        seen_mask |= (h[0] < 32) ? (1u << h[0]) : 0;
        if (h[0] == MB2_TAG_FRAMEBUFFER && h[1] >= sizeof(mb2_fb_tag_t)) { tag = (const mb2_fb_tag_t*)p; break; }
        p += (h[1] + 7) & ~7u;
    }
    if (!tag) {
        uart_print("[KIRA FB] No framebuffer tag from the bootloader (MB2 tag mask ");
        uart_print_hex(seen_mask);
        uart_print("): text mode only, serial console stays.\n");
        return -2;
    }
    uart_print("[KIRA FB] Bootloader mode: ");
    uart_print_dec(tag->width); uart_print("x"); uart_print_dec(tag->height);
    uart_print("x"); uart_print_dec(tag->bpp);
    uart_print(" type "); uart_print_dec(tag->fb_type);
    uart_print(" addr "); uart_print_hex(tag->addr); uart_print("\n");
    if (tag->fb_type != 1) {
        uart_print("[KIRA FB] Bootloader gave a non-RGB (text/indexed) framebuffer: unsupported, serial only.\n");
        return -3;
    }
    fmt = (fbpix_fmt_t){ (uint8_t)((tag->bpp + 7) / 8), tag->red_pos, tag->red_size, tag->green_pos, tag->green_size, tag->blue_pos, tag->blue_size };
    if (fbpix_valid(&fmt, tag->bpp) != 0) {
        uart_print("[KIRA FB] Unsupported pixel format (need 15/16/24/32 bpp RGB): serial only.\n");
        return -3;
    }
    native = fbpix_is_native(&fmt);
    uint32_t w = tag->width, h = tag->height, pitch = tag->pitch;
    if (w < 320 || h < 200 || w > 4096 || h > 4096 || pitch < w * fmt.bytes || (w & 1)) {
        uart_print("[KIRA FB] Unreasonable framebuffer geometry: serial only.\n");
        return -3;
    }

    uint64_t phys = tag->addr;
    uint64_t bytes = (uint64_t)pitch * h;
    // Видеопамять - в окно MMIO ядра, где бы она ни лежала: write-combining (PAT), иначе без кэша.
    // Через identity-карту (write-back) записи могли бы застрять в кэше и не дойти до экрана.
    int wc = (vmm_enable_write_combining() == 0);
    front = (uint8_t*)(uintptr_t)vmm_map_mmio(phys, bytes, VMM_MMIO_WC);
    if (!front) {
        uart_print("[KIRA FB] Cannot map video memory: serial only.\n");
        return -4;
    }

    uint32_t back_bytes = w * h * 4;
    uint32_t pages = (back_bytes + 4095) / 4096;
    uint64_t bb = pmm_alloc_contiguous(pages);
    if (!bb) return -5;

    info.phys = phys; info.width = w; info.height = h; info.pitch_bytes = pitch; info.bpp = tag->bpp;
    info.write_combining = wc; info.back_phys = bb; info.back_bytes = back_bytes;
    back.px = (uint32_t*)(uintptr_t)bb; back.w = (int)w; back.h = (int)h; back.pitch = (int)w;
    gfx_fill(&back, 0x000000);

    ready = 1;
    uart_print("[KIRA FB] ");
    uart_print_dec(w); uart_print("x"); uart_print_dec(h); uart_print("x"); uart_print_dec(tag->bpp); uart_print(" linear framebuffer at ");
    uart_print_hex(phys);
    uart_print(wc ? ", write-combining (PAT)" : ", uncached (no PAT)");
    if (!native) { uart_print("; "); uart_print_dec(tag->bpp); uart_print(" bpp - pixels converted on present"); }
    uart_print("; back buffer ");
    uart_print_dec(back_bytes >> 10);
    uart_print(" KB in RAM.\n");
    return 0;
}

static void copy_row(uint8_t* dst, const uint8_t* src, uint32_t px) {
    uint64_t q = px / 2;
    gfx_copy64(dst, src, q);
    if (px & 1) ((uint32_t*)dst)[px - 1] = ((const uint32_t*)src)[px - 1];
}

void fb_present_rect(int x, int y, int w, int h) {
    if (!ready) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > back.w) x1 = back.w;
    if (y1 > back.h) y1 = back.h;
    if (x0 >= x1 || y0 >= y1) return;
    if (!native) {                                 // 24/16/15 бит: перевод каждой строки
        for (int j = y0; j < y1; j++)
            fbpix_row(front + (uint64_t)j * info.pitch_bytes + (uint64_t)x0 * fmt.bytes, back.px + (long)j * back.pitch + x0, (uint32_t)(x1 - x0), &fmt);
        return;
    }
    if (flush_hook) {
        uint64_t f = spin_lock_irqsave(&dlk);
        if (x0 < dx0) dx0 = x0;
        if (y0 < dy0) dy0 = y0;
        if (x1 > dx1) dx1 = x1;
        if (y1 > dy1) dy1 = y1;
        spin_unlock_irqrestore(&dlk, f);
    }
    x0 &= ~1;                                     // выравнивание по 8 байт для rep movsq
    for (int j = y0; j < y1; j++) {
        copy_row(front + (uint64_t)j * info.pitch_bytes + (uint64_t)x0 * 4,
                 (const uint8_t*)(back.px + (long)j * back.pitch + x0), (uint32_t)(x1 - x0));
    }
}

void fb_frame_done(void) {
    if (!ready || !flush_hook) return;
    uint64_t f = spin_lock_irqsave(&dlk);
    int x0 = dx0, y0 = dy0, x1 = dx1, y1 = dy1;
    dx0 = dy0 = 1 << 30; dx1 = dy1 = 0;
    spin_unlock_irqrestore(&dlk, f);
    if (x1 > x0 && y1 > y0) flush_hook(x0, y0, x1 - x0, y1 - y0);
}

int fb_attach_linear(uint64_t phys, void* fr, uint32_t w, uint32_t h, uint32_t pitch, void (*flush)(int, int, int, int)) {
    if (ready || w < 320 || h < 200 || pitch < w * 4) return -1;
    fmt = (fbpix_fmt_t){ 4, 16, 8, 8, 8, 0, 8 };
    native = 1;
    front = (uint8_t*)fr;
    uint32_t back_bytes = w * h * 4;
    uint64_t bb = pmm_alloc_contiguous((back_bytes + 4095) / 4096);
    if (!bb) return -5;
    info.phys = phys; info.width = w; info.height = h; info.pitch_bytes = pitch; info.bpp = 32;
    info.write_combining = 0; info.back_phys = bb; info.back_bytes = back_bytes;
    back.px = (uint32_t*)(uintptr_t)bb; back.w = (int)w; back.h = (int)h; back.pitch = (int)w;
    gfx_fill(&back, 0x000000);
    flush_hook = flush;
    ready = 1;
    return 0;
}

void fb_present(void) {
    if (!ready) return;
    if (native && info.pitch_bytes == info.width * 4 && !flush_hook) {
        gfx_copy64(front, back.px, (uint64_t)info.width * info.height / 2);
    } else {
        fb_present_rect(0, 0, (int)info.width, (int)info.height);
        fb_frame_done();
    }
}
