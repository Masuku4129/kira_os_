#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

// Запускает PIT на заданной частоте (19..1193182 Гц), регистрирует IRQ0 и
// размаскирует его. Возвращает 0 при успехе. Прерывания глобально включает
// вызывающий код (sti).
int timer_init(uint32_t frequency);
void timer_handler(void);
#ifndef TIMER_HZ
#define TIMER_HZ 500        // частота тиков планировщика и часов: 2 мс, кадр 60 к/с = 8 тиков; дешевле 1000 Гц на эмуляции
#endif
// Программы и «человеческие» величины считают в тиках по 10 мс (прежние 100 Гц): ABI не меняется при смене частоты.
#define TIMER_PER_10MS (TIMER_HZ / 100)
uint64_t timer_get_ticks(void);
uint64_t timer_get_irqs(void);   // сколько прерываний таймера реально пришло (тики при потерях догоняются по TSC)

#endif
