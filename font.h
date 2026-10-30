#ifndef KIRA_FONT_H
#define KIRA_FONT_H

#include <stdint.h>

#define FONT_W 8
#define FONT_H 16

// 16 байт: строки глифа сверху вниз, старший бит - левый пиксель.
// Для символов вне таблицы возвращается рамка-заменитель (никогда не NULL).
const uint8_t* font_glyph(uint32_t codepoint);

#endif
