#!/usr/bin/env python3
"""Таблица ядра фильтра понижения частоты микшера (audio_fir.h). Запуск: python3 -I tools/gen_audio_fir.py > audio_fir.h

Ядро - оконный sinc с окном Кайзера, в единицах ВЫХОДНОГО кадра (ось x - в кадрах 48 кГц):
    k(x) = 2*fc * sinc(2*fc*x) * kaiser(x / HALF),   fc = 0.45 циклов на выходной кадр (середина перехода 0.40 .. 0.50)
Полоса пропускания - до 0.40 частоты дискретизации выхода (19,2 кГц), полоса подавления - от 0.5 (24 кГц, новая
частота Найквиста). Полуширина HALF = 20 выходных кадров, шаг таблицы 1/RES выходного кадра (между узлами - линейная
интерполяция в audio.c), значения - Q15. Проверка таблицы по этой же формуле - tests/audio_test.c."""
import math

HALF, RES, FC, BETA = 20, 256, 0.45, 5.0


def i0(x):
    s, t, k = 1.0, 1.0, 1
    while t > 1e-18 * s:
        t *= (x / (2 * k)) ** 2
        s += t
        k += 1
    return s


def kernel(x):
    if abs(x) >= HALF:
        return 0.0
    sinc = 1.0 if x == 0 else math.sin(2 * math.pi * FC * x) / (2 * math.pi * FC * x)
    win = i0(BETA * math.sqrt(1 - (x / HALF) ** 2)) / i0(BETA)
    return 2 * FC * sinc * win


print("/* Создан tools/gen_audio_fir.py - не править руками. Ядро фильтра понижения частоты: окно Кайзера (beta %g) x sinc с fc %g," % (BETA, FC))
print("   полуширина %d выходных кадров, %d узлов на кадр, Q15. */" % (HALF, RES))
print("#ifndef KIRA_AUDIO_FIR_H\n#define KIRA_AUDIO_FIR_H")
print("#define AUDIO_FIR_HALF %d\n#define AUDIO_FIR_RES %d" % (HALF, RES))
print("static const int16_t audio_fir_tab[AUDIO_FIR_HALF * AUDIO_FIR_RES + 1] = {")
vals = [int(round(32768 * kernel(j / RES))) for j in range(HALF * RES + 1)]
for i in range(0, len(vals), 16):
    print("    " + ", ".join(str(v) for v in vals[i:i + 16]) + ",")
print("};\n#endif")
