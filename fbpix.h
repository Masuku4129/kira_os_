#ifndef FBPIX_H
#define FBPIX_H
#include <stdint.h>

/* Формат пикселя видеопамяти и перевод строки заднего буфера (0x00RRGGBB) в него.
   32 бита 0x00RRGGBB копируются как есть (быстрый путь в fb.c); 24, 16 и 15 бит переводятся попиксельно:
   так экран работает и на видеокартах/эмуляторах, которые не дают 32-битного режима (cirrus, часть VBE). */
typedef struct { uint8_t bytes, rp, rs, gp, gs, bp, bs; } fbpix_fmt_t;

/* 0 - формат понятен (каналы 1..8 бит, внутри пикселя, 2..4 байта на пиксель). */
static inline int fbpix_valid(const fbpix_fmt_t* f, unsigned bpp) {
    if (f->bytes < 2 || f->bytes > 4 || bpp > f->bytes * 8u) return -1;
    if (!f->rs || f->rs > 8 || !f->gs || f->gs > 8 || !f->bs || f->bs > 8) return -1;
    if (f->rp + f->rs > bpp || f->gp + f->gs > bpp || f->bp + f->bs > bpp) return -1;
    return 0;
}

static inline int fbpix_is_native(const fbpix_fmt_t* f) {
    return f->bytes == 4 && f->rp == 16 && f->rs == 8 && f->gp == 8 && f->gs == 8 && f->bp == 0 && f->bs == 8;
}

static inline void fbpix_row(uint8_t* d, const uint32_t* s, uint32_t n, const fbpix_fmt_t* f) {
    unsigned rsh = 8u - f->rs, gsh = 8u - f->gs, bsh = 8u - f->bs;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = s[i];
        uint32_t v = (((c >> 16) & 255u) >> rsh) << f->rp | (((c >> 8) & 255u) >> gsh) << f->gp | ((c & 255u) >> bsh) << f->bp;
        if (f->bytes == 4) { d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); d[2] = (uint8_t)(v >> 16); d[3] = (uint8_t)(v >> 24); d += 4; }
        else if (f->bytes == 3) { d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); d[2] = (uint8_t)(v >> 16); d += 3; }
        else { d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); d += 2; }
    }
}

#endif
