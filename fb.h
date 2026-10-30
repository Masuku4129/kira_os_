#ifndef FB_H
#define FB_H

#include <stdint.h>
#include "gfx2d.h"

typedef struct {
    uint64_t phys;          // физический адрес видеопамяти (LFB)
    uint32_t width, height, pitch_bytes, bpp;
    int write_combining;    // 1 - отображена как WC (быстро), 0 - WB/UC
    uint64_t back_phys;     // задний буфер
    uint32_t back_bytes;
} fb_info_t;

// Разбирает тег фреймбуфера Multiboot2, отображает видеопамять (PAT: write-combining),
// заводит задний буфер в обычной памяти. 0 - успех; отрицательное - экрана нет
// (текстовый режим, неподдерживаемый формат пикселя, нет памяти): ядро продолжает на UART.
int fb_init(unsigned long mbi_addr);
int fb_available(void);
const fb_info_t* fb_get_info(void);

// Задний буфер: рисуют сюда, потом показывают fb_present*. Формат 0x00RRGGBB.
surface_t* fb_surface(void);
void fb_present(void);
void fb_present_rect(int x, int y, int w, int h);

#endif
