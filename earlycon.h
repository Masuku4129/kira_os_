#ifndef EARLYCON_H
#define EARLYCON_H

#include <stdint.h>

// Ранняя консоль: выводит журнал ядра прямо в видеопамять, без окон, куч и потоков.
// Работает с первой строки загрузки, поэтому любой сбой на реальном железе виден на экране,
// а не только в последовательном порту. Режимы:
//   RGB  - линейный буфер кадра 32 бит (порядок каналов берётся из тега загрузчика);
//   TEXT - текстовый режим VGA 80x25 (BIOS-загрузка без графики, EGA-тег загрузчика).
// Экран пишется кольцом: новая строка затирает самую старую, под последней строкой рисуется черта.
typedef struct {
    uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
} earlycon_pixfmt_t;

// Подключение к готовой видеопамяти (используется и тестами).
int  earlycon_attach_rgb(void* base, uint32_t width, uint32_t height, uint32_t pitch_bytes, const earlycon_pixfmt_t* fmt);
int  earlycon_attach_text(void* base, uint32_t cols, uint32_t rows);
// Разбор тега загрузчика, отображение видеопамяти, повтор уже накопленного журнала. 0 - работает.
int  earlycon_init(unsigned long mbi);
void earlycon_putc(char c);        // вызывается из uart_putchar
void earlycon_alert(void);         // дальнейший текст - красным (паника, фатальное исключение)
void earlycon_stop(void);          // отключить и снять отображения (до того, как экран возьмёт драйвер дисплея)
int  earlycon_active(void);
void earlycon_geometry(uint32_t* cols, uint32_t* rows);
void earlycon_cursor(uint32_t* cx, uint32_t* cy);

#endif
