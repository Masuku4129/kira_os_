#include "wallpaper.h"

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int wallpaper_parse(const uint8_t* buf, size_t len, int* w, int* h, const uint8_t** rgb) {
    if (!buf || len < 12 || buf[0] != 'K' || buf[1] != 'W' || buf[2] != 'P' || buf[3] != '1') return -1;
    uint32_t iw = rd32(buf + 4), ih = rd32(buf + 8);
    if (iw < 2 || ih < 2 || iw > WALLPAPER_MAX_DIM || ih > WALLPAPER_MAX_DIM) return -1;
    if ((uint64_t)len < 12 + (uint64_t)iw * ih * 3) return -1;
    *w = (int)iw; *h = (int)ih; *rgb = buf + 12;
    return 0;
}

void wallpaper_scale(uint32_t* px, int w, int h, const uint8_t* rgb, int iw, int ih) {
    if (!px || !rgb || w < 1 || h < 1 || iw < 2 || ih < 2) return;
    /* «заполнить»: масштаб s = max(w/iw, h/ih); окно источника по центру, в 16.16 */
    uint64_t sx16, sy16;                                  /* шаг по источнику на одну точку экрана, 16.16 */
    uint64_t cw, ch;                                      /* размер окна источника, 16.16 */
    if ((uint64_t)w * ih >= (uint64_t)h * iw) { cw = (uint64_t)iw << 16; ch = (cw * h) / w; }
    else { ch = (uint64_t)ih << 16; cw = (ch * w) / h; }
    sx16 = cw / w; sy16 = ch / h;
    uint64_t ox = (((uint64_t)iw << 16) - cw) / 2, oy = (((uint64_t)ih << 16) - ch) / 2;
    for (int y = 0; y < h; y++) {
        uint64_t fy = oy + sy16 * y + sy16 / 2;
        fy = fy >= 32768 ? fy - 32768 : 0;                /* центр точки источника - в её середине */
        int y0 = (int)(fy >> 16); uint32_t ty = (uint32_t)(fy & 0xFFFF) >> 8;
        if (y0 > ih - 2) { y0 = ih - 2; ty = 255; }
        const uint8_t* r0 = rgb + (size_t)y0 * iw * 3; const uint8_t* r1 = r0 + (size_t)iw * 3;
        for (int x = 0; x < w; x++) {
            uint64_t fx = ox + sx16 * x + sx16 / 2;
            fx = fx >= 32768 ? fx - 32768 : 0;
            int x0 = (int)(fx >> 16); uint32_t tx = (uint32_t)(fx & 0xFFFF) >> 8;
            if (x0 > iw - 2) { x0 = iw - 2; tx = 255; }
            const uint8_t* a = r0 + x0 * 3; const uint8_t* b = r1 + x0 * 3;
            uint32_t c[3];
            for (int k = 0; k < 3; k++) {
                uint32_t top = a[k] * (256 - tx) + a[3 + k] * tx;
                uint32_t bot = b[k] * (256 - tx) + b[3 + k] * tx;
                c[k] = (top * (256 - ty) + bot * ty + 32768) >> 16;
                if (c[k] > 255) c[k] = 255;
            }
            px[(size_t)y * w + x] = c[0] << 16 | c[1] << 8 | c[2];
        }
    }
}
