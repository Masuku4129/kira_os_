#ifndef KPAR_H
#define KPAR_H
#include <stdint.h>

/* Параллельное выполнение внутри ядра: kpar_run(fn, ctx, n) выполняет fn(ctx, 0..n-1) на нескольких ядрах
   (помощники + сам вызвавший поток) и возвращается, когда выполнены все n частей. Части должны быть независимы
   (разные строки экрана, разные блоки данных). Помощников столько, сколько ядер сверх одного (не больше KPAR_MAX).
   Без помощников (одно ядро, хост-тест) - обычный цикл. Вызывать из одного потока за раз (композитор). */
#define KPAR_MAX 31
typedef void (*kpar_fn)(void* ctx, uint32_t i);

void     kpar_init(unsigned helpers);             /* после запуска планировщика */
void     kpar_run(kpar_fn fn, void* ctx, uint32_t n);
unsigned kpar_helpers(void);

#endif
