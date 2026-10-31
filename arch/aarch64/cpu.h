#ifndef CPU_H
#define CPU_H
#include <stdint.h>

/* ARM64: счётчик времени - виртуальный счётчик общего таймера (имя rdtsc сохранено для общего кода; частота -
   CNTFRQ_EL0, её сообщает clock). Прерывания - бит I в DAIF. */
static inline uint64_t rdtsc(void) {
    uint64_t v;
    __asm__ volatile ("isb\n mrs %0, cntvct_el0" : "=r"(v) : : "memory");
    return v;
}

static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile ("mrs %0, daif\n msr daifset, #2" : "=r"(f) : : "memory");
    return f;
}
static inline void irq_restore(uint64_t f) {
    if (!(f & (1u << 7))) __asm__ volatile ("msr daifclr, #2" : : : "memory");
}

/* Регистры SIMD/FP потока: q0..q31, FPCR, FPSR (fpu.S). */
void fpu_save(void* area);
void fpu_restore(const void* area);

static inline void cpu_halt(void) { __asm__ volatile ("wfi" ::: "memory"); }
static inline void cpu_irq_off(void) { __asm__ volatile ("msr daifset, #2" ::: "memory"); }
static inline void cpu_irq_on(void) { __asm__ volatile ("msr daifclr, #2" ::: "memory"); }
static inline void cpu_relax(void) { __asm__ volatile ("yield" ::: "memory"); }
/* Барьер памяти для DMA и регистров устройств: запись в буфер должна стать видна устройству до «звонка» в его
   регистр, а чтение регистра - завершиться до чтения буфера (на ARM порядок слабый). */
static inline void cpu_mb(void) { __asm__ volatile ("dsb sy" ::: "memory"); }
/* Пауза ~1 мкс по счётчику общего таймера (он идёт с включения, калибровать не нужно). */
static inline void cpu_io_delay(void) {
    uint64_t f, t0 = rdtsc();
    __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f));
    uint64_t need = f / 1000000 ? f / 1000000 : 1;
    while (rdtsc() - t0 < need) { }
}
static inline int cpu_irq_enabled(void) { uint64_t f; __asm__ volatile ("mrs %0, daif" : "=r"(f)); return !(f & (1u << 7)); }

#endif
