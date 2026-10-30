#ifndef CLOCK_H
#define CLOCK_H

#include <stdint.h>

// Монотонное время с наносекундным разрешением на основе TSC.
// Тик PIT (10 мс) слишком груб для кадров анимации и игровых таймеров;
// TSC даёт разрешение в такты процессора.
void clock_init(uint64_t tsc_khz);      // после калибровки TSC
int clock_ready(void);
uint64_t clock_tsc_khz(void);           // частота TSC в кГц; 0 - не откалибрована
uint64_t clock_ns(void);                // наносекунд с clock_init
uint64_t clock_ms(void);
uint64_t clock_tsc_to_ns(uint64_t cycles, uint64_t tsc_khz); // чистая функция (для тестов)
uint64_t clock_khz_from_cpuid15(uint32_t eax_den, uint32_t ebx_num, uint32_t ecx_hz);   // 0 - лист не даёт частоты

// Частота TSC в кГц без тика и прерываний: канал 2 PIT (медиана трёх замеров по 50 мс), сверка с CPUID 0x15;
// без PIT - CPUID 0x15, затем приблизительно CPUID 0x16. *method - чем измерено. 0 - измерить нечем.
uint64_t clock_calibrate(const char** method);

#endif
