// Регулятор анимации фона рабочего стола. Чистая логика без ядра: раз в секунду получает число кадров и опоздавших
// кадров цикла дисплея и решает уровень анимации фона. Темп окон программ он не трогает: открытые окна идут в полный
// темп экрана, свёрнутые ограничены в gfxsrv.c. Разум в этом не участвует: решение детерминированное, без обучения.
#ifndef FRAMEGOV_H
#define FRAMEGOV_H
#include <stdint.h>

#define FGOV_FX_AUTO 4          // значение настройки анимации «решает регулятор»

typedef struct {
    uint32_t bad, fxcalm;               // плохие и спокойные секунды подряд
    uint32_t fx;                         // текущий уровень анимации 0..3
} framegov_t;

void framegov_init(framegov_t* g);
// frames, late - кадров и опоздавших за последнюю секунду; fx_setting: 0..3 вручную или FGOV_FX_AUTO.
void framegov_second(framegov_t* g, uint32_t frames, uint32_t late, int fx_setting);

#endif
