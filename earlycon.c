#include "earlycon.h"
#include "bootmap.h"
#include "mbinfo.h"
#include "klog.h"
#include "font.h"
#include "uart.h"
#include "cpu.h"
#include "spinlock.h"

#define MB2_TAG_FRAMEBUFFER 8
#define VGA_TEXT_ADDR 0xB8000ULL

typedef struct {
    uint32_t type, size;
    uint64_t addr;
    uint32_t pitch, width, height;
    uint8_t bpp, fb_type;
    uint16_t reserved;
    uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
} __attribute__((packed)) mb2_fb_tag_t;

enum { EC_NONE = 0, EC_RGB = 1, EC_TEXT = 2 };

static spinlock_t ec_lock = SPINLOCK_INIT;

static struct {
    int mode;
    uint8_t* base;
    uint32_t width, height, pitch;      // RGB: пиксели и байты на строку
    uint32_t scale;                     // RGB: целочисленное увеличение знакоместа
    uint32_t cols, rows;
    uint32_t cx, cy;
    int wrapped;
    uint32_t fg, bg, fg_normal, fg_alert;   // RGB: готовые значения пикселя
    uint8_t attr_normal, attr_alert, attr;  // TEXT
    uint32_t cp, pending;               // разбор UTF-8
} ec;

static uint32_t pack(const earlycon_pixfmt_t* f, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)(r >> (8 - f->red_size)) << f->red_pos) |
           ((uint32_t)(g >> (8 - f->green_size)) << f->green_pos) |
           ((uint32_t)(b >> (8 - f->blue_size)) << f->blue_pos);
}

// ---- рисование ----------------------------------------------------------

static void rgb_fill_lines(uint32_t y0, uint32_t y1, uint32_t color) {
    for (uint32_t y = y0; y < y1 && y < ec.height; y++) {
        volatile uint32_t* px = (volatile uint32_t*)(ec.base + (uint64_t)y * ec.pitch);
        for (uint32_t x = 0; x < ec.width; x++) px[x] = color;
    }
}

static void rgb_clear_row(uint32_t row) {
    uint32_t h = FONT_H * ec.scale;
    rgb_fill_lines(row * h, row * h + h, ec.bg);
}

static void rgb_rule_row(uint32_t row) {   // тонкая черта в верхней части строки: «дальше - старое»
    uint32_t h = FONT_H * ec.scale;
    rgb_fill_lines(row * h, row * h + 2 * ec.scale, ec.fg_normal);
}

static void rgb_draw(uint32_t col, uint32_t row, uint32_t cp) {
    const uint8_t* g = font_glyph(cp);
    uint32_t s = ec.scale;
    uint8_t* base = ec.base + (uint64_t)row * FONT_H * s * ec.pitch + (uint64_t)col * FONT_W * s * 4;
    for (uint32_t y = 0; y < FONT_H; y++) {
        uint8_t bits = g[y];
        for (uint32_t sy = 0; sy < s; sy++) {
            volatile uint32_t* px = (volatile uint32_t*)(base + (uint64_t)(y * s + sy) * ec.pitch);
            for (uint32_t x = 0; x < FONT_W; x++) {
                uint32_t c = (bits & (0x80u >> x)) ? ec.fg : ec.bg;
                for (uint32_t sx = 0; sx < s; sx++) *px++ = c;
            }
        }
    }
}

static volatile uint16_t* text_cell(uint32_t col, uint32_t row) {
    return (volatile uint16_t*)ec.base + (uint64_t)row * ec.cols + col;
}

static void text_clear_row(uint32_t row, char fill) {
    for (uint32_t x = 0; x < ec.cols; x++) *text_cell(x, row) = (uint16_t)(((uint16_t)ec.attr_normal << 8) | (uint8_t)fill);
}

static void clear_row(uint32_t row) {
    if (ec.mode == EC_RGB) rgb_clear_row(row); else text_clear_row(row, ' ');
}

static void mark_row(uint32_t row) {
    if (ec.mode == EC_RGB) { rgb_clear_row(row); rgb_rule_row(row); }
    else text_clear_row(row, '-');
}

static void clear_all(void) {
    for (uint32_t r = 0; r < ec.rows; r++) clear_row(r);
    ec.cx = ec.cy = 0;
    ec.wrapped = 0;
}

static void newline(void) {
    ec.cx = 0;
    ec.cy++;
    if (ec.cy >= ec.rows) { ec.cy = 0; ec.wrapped = 1; }
    if (ec.wrapped) {
        clear_row(ec.cy);
        mark_row((ec.cy + 1) % ec.rows);
    }
}

static void put_glyph(uint32_t cp) {
    if (ec.cx >= ec.cols) newline();
    if (ec.mode == EC_RGB) rgb_draw(ec.cx, ec.cy, cp);
    else *text_cell(ec.cx, ec.cy) = (uint16_t)(((uint16_t)ec.attr << 8) | (cp < 0x80 ? (uint8_t)cp : (uint8_t)'?'));
    ec.cx++;
}

static void emit(uint32_t cp) {
    switch (cp) {
    case '\n': newline(); return;
    case '\r': ec.cx = 0; return;
    case '\f': clear_all(); return;
    case '\b': if (ec.cx > 0) ec.cx--; return;
    case '\t': { uint32_t n = 8 - (ec.cx & 7); while (n--) put_glyph(' '); return; }
    default: break;
    }
    if (cp < 0x20 || cp == 0x7F) return;
    put_glyph(cp);
}

void earlycon_putc(char ch) {
    if (ec.mode == EC_NONE) return;
    uint64_t f = spin_lock_irqsave(&ec_lock);
    uint8_t c = (uint8_t)ch;
    if (c < 0x80) { ec.pending = 0; emit(c); }
    else if ((c & 0xC0) == 0x80) {
        if (ec.pending) {
            ec.cp = (ec.cp << 6) | (c & 0x3Fu);
            if (--ec.pending == 0) emit(ec.cp);
        }
    } else if ((c & 0xE0) == 0xC0) { ec.cp = c & 0x1Fu; ec.pending = 1; }
    else if ((c & 0xF0) == 0xE0)   { ec.cp = c & 0x0Fu; ec.pending = 2; }
    else if ((c & 0xF8) == 0xF0)   { ec.cp = c & 0x07u; ec.pending = 3; }
    else ec.pending = 0;
    spin_unlock_irqrestore(&ec_lock, f);
}

void earlycon_alert(void) {
    if (ec.mode == EC_RGB) ec.fg = ec.fg_alert;
    else if (ec.mode == EC_TEXT) ec.attr = ec.attr_alert;
}

// ---- подключение --------------------------------------------------------

static void replay_log(void) {
    uint64_t cursor = 0;
    char buf[128];
    unsigned lost, n;
    while ((n = klog_read(&cursor, buf, sizeof buf, &lost)) > 0) {
        for (unsigned i = 0; i < n; i++) earlycon_putc(buf[i]);
    }
}

int earlycon_attach_rgb(void* base, uint32_t width, uint32_t height, uint32_t pitch_bytes, const earlycon_pixfmt_t* fmt) {
    if (!base || !fmt || width < 320 || height < 200 || pitch_bytes < width * 4) return -1;
    if (!fmt->red_size || !fmt->green_size || !fmt->blue_size ||
        fmt->red_size > 8 || fmt->green_size > 8 || fmt->blue_size > 8) return -1;
    ec.mode = EC_NONE;
    ec.base = (uint8_t*)base;
    ec.width = width; ec.height = height; ec.pitch = pitch_bytes;
    ec.scale = width / 1280; if (ec.scale < 1) ec.scale = 1; if (ec.scale > 4) ec.scale = 4;
    ec.cols = width / (FONT_W * ec.scale);
    ec.rows = height / (FONT_H * ec.scale);
    ec.bg = pack(fmt, 0x00, 0x00, 0x00);
    ec.fg_normal = pack(fmt, 0xE0, 0xE0, 0xE0);
    ec.fg_alert = pack(fmt, 0xFF, 0x60, 0x60);
    ec.fg = ec.fg_normal;
    ec.pending = 0;
    ec.mode = EC_RGB;
    rgb_fill_lines(0, height, ec.bg);
    ec.cx = ec.cy = 0; ec.wrapped = 0;
    return 0;
}

int earlycon_attach_text(void* base, uint32_t cols, uint32_t rows) {
    if (!base || cols < 20 || rows < 5 || cols > 256 || rows > 100) return -1;
    ec.mode = EC_NONE;
    ec.base = (uint8_t*)base;
    ec.cols = cols; ec.rows = rows;
    ec.attr_normal = 0x07; ec.attr_alert = 0x0C; ec.attr = ec.attr_normal;
    ec.pending = 0;
    ec.mode = EC_TEXT;
    clear_all();
    return 0;
}

// Окно bootmap нужно и здесь: mbinfo_relocate ставит его только когда Multiboot2-структура лежит выше 1 ГБ.
static int window_ready(void) {
    if (bootmap_init() == 0) return 1;
    uart_print("[KIRA EARLYCON] Cannot install the early window (page table slot busy): no early console on screen.\n");
    return 0;
}

int earlycon_init(unsigned long mbi) {
    const mb2_fb_tag_t* t = (const mb2_fb_tag_t*)mbinfo_find_tag(mbi, MB2_TAG_FRAMEBUFFER);
    int rc;
    if (!t) {
        // Загрузчик не дал буфер кадра: остаётся текстовый режим, который BIOS и GRUB оставили включённым.
        rc = earlycon_attach_text((void*)(uintptr_t)VGA_TEXT_ADDR, 80, 25);
    } else if (t->size < 32) {
        uart_print("[KIRA EARLYCON] Truncated framebuffer tag: no early console on screen.\n");
        return -3;
    } else if (t->size >= sizeof(*t) && t->fb_type == 1 && t->bpp == 32) {
        uint64_t bytes = (uint64_t)t->pitch * t->height;
        if (!window_ready()) return -2;
        void* va = bytes ? bootmap_map(t->addr, bytes, 1) : 0;
        if (!va) { uart_print("[KIRA EARLYCON] Cannot map video memory for the early console.\n"); return -2; }
        earlycon_pixfmt_t f = { t->red_pos, t->red_size, t->green_pos, t->green_size, t->blue_pos, t->blue_size };
        rc = earlycon_attach_rgb(va, t->width, t->height, t->pitch, &f);
    } else if (t->fb_type == 2 && t->width >= 20 && t->height >= 5) {
        uint32_t cols = t->width, rows = t->height;
        if (!window_ready()) return -2;
        void* va = bootmap_map(t->addr, (uint64_t)cols * rows * 2, 1);
        if (!va) { uart_print("[KIRA EARLYCON] Cannot map the text buffer.\n"); return -2; }
        rc = earlycon_attach_text(va, cols, rows);
    } else {
        uart_print("[KIRA EARLYCON] Unsupported bootloader framebuffer (type ");
        uart_print_dec(t->fb_type);
        uart_print(", bpp ");
        uart_print_dec(t->bpp);
        uart_print("): no early console on screen.\n");
        return -3;
    }
    if (rc != 0) return rc;
    replay_log();
    return 0;
}

void earlycon_stop(void) {
    ec.mode = EC_NONE;
    bootmap_release_all();
}

int earlycon_active(void) { return ec.mode != EC_NONE; }
void earlycon_geometry(uint32_t* cols, uint32_t* rows) { if (cols) *cols = ec.cols; if (rows) *rows = ec.rows; }
void earlycon_cursor(uint32_t* cx, uint32_t* cy) { if (cx) *cx = ec.cx; if (cy) *cy = ec.cy; }
