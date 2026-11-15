#include "framegov.h"

// Ступени темпа фоновых окон. Опоздавшие кадры за секунду - ступень ниже; две спокойные секунды - ступень выше.
static const uint8_t steps[] = { 0, 30, 15, 8, 4 };
#define NSTEPS ((uint32_t)(sizeof steps / sizeof steps[0]))

void framegov_init(framegov_t* g) {
    g->lvl = g->calm = g->bad = g->fxcalm = 0;
    g->fx = 3; g->bg_fps = 0;
}

int framegov_second(framegov_t* g, uint32_t frames, uint32_t late, int fx_setting) {
    if (!frames) return 0;                                  // цикл не крутился - сравнивать не с чем
    if (late > frames) late = frames;
    uint32_t pct = late * 100 / frames;
    uint32_t lvl = g->lvl;
    if (late) { g->calm = 0; if (lvl + 1 < NSTEPS) lvl++; }
    else if (++g->calm >= 2 && lvl) { lvl--; g->calm = 0; }
    int changed = lvl != g->lvl;
    if (changed) { g->lvl = lvl; g->bg_fps = steps[lvl]; }
    // Анимация: вручную - как задано; авто - 3 секунды с 10% опоздавших уровнем ниже, 30 спокойных секунд уровнем выше.
    if (fx_setting == FGOV_FX_AUTO) {
        if (pct >= 10) { g->fxcalm = 0; if (++g->bad >= 3 && g->fx > 0) { g->fx--; g->bad = 0; } }
        else { g->bad = 0; if (!late && ++g->fxcalm >= 30 && g->fx < 3) { g->fx++; g->fxcalm = 0; } }
    } else { g->fx = fx_setting < 0 ? 0 : fx_setting > 3 ? 3 : (uint32_t)fx_setting; g->bad = g->fxcalm = 0; }
    return changed;
}
