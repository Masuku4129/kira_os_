#include "framegov.h"

void framegov_init(framegov_t* g) { g->bad = g->fxcalm = 0; g->fx = 3; }

void framegov_second(framegov_t* g, uint32_t frames, uint32_t late, int fx_setting) {
    if (!frames) return;                                    // цикл не крутился - сравнивать не с чем
    if (late > frames) late = frames;
    uint32_t pct = late * 100 / frames;
    // Вручную - как задано; авто - 3 секунды с 10% опоздавших уровнем ниже, 30 спокойных секунд уровнем выше.
    if (fx_setting == FGOV_FX_AUTO) {
        if (pct >= 10) { g->fxcalm = 0; if (++g->bad >= 3 && g->fx > 0) { g->fx--; g->bad = 0; } }
        else { g->bad = 0; if (!late && ++g->fxcalm >= 30 && g->fx < 3) { g->fx++; g->fxcalm = 0; } }
    } else { g->fx = fx_setting < 0 ? 0 : fx_setting > 3 ? 3 : (uint32_t)fx_setting; g->bad = g->fxcalm = 0; }
}
