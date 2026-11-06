#ifndef ICONS_H
#define ICONS_H
#include <stdint.h>
#include "gfx2d.h"

/* Неоновые значки: рисуются сглаженными линиями прямо в поверхность, с мягким свечением.
   (cx, cy) - центр, size - сторона квадрата значка в пикселях. */
enum { ICON_TERMINAL = 1, ICON_FOLDER, ICON_FILE, ICON_AI, ICON_BRAIN, ICON_MONITOR, ICON_WAVE, ICON_BRUSH,
       ICON_CUBE, ICON_GEAR, ICON_HOME, ICON_CHIP,
       ICON_EDIT, ICON_EYE, ICON_DOWNLOAD, ICON_SERVER, ICON_MIC, ICON_NET, ICON_BRACES, ICON_WASM, ICON_DRVGET,
       ICON_FLAME, ICON_NOTE, ICON_MORE, ICON_LAST };

void icon_draw(surface_t* s, int cx, int cy, int size, int kind, uint32_t color, int glow);

#endif
