#ifndef CPUMASK_H
#define CPUMASK_H
#include <stdint.h>

/* Набор ядер (маска сродства потока, адресаты рассылки). До CPU_MAX ядер: x2APIC адресует и больше, предел
   задан размером таблицы ядер (percpu) - она статическая, потому что нужна до запуска кучи. */
#define CPU_MAX       256
#define CPUMASK_WORDS (CPU_MAX / 64)

typedef struct { uint64_t w[CPUMASK_WORDS]; } cpumask_t;

static inline cpumask_t cm_none(void) { cpumask_t m; for (int i = 0; i < CPUMASK_WORDS; i++) m.w[i] = 0; return m; }
static inline cpumask_t cm_all(void) { cpumask_t m; for (int i = 0; i < CPUMASK_WORDS; i++) m.w[i] = ~0ULL; return m; }
static inline cpumask_t cm_one(unsigned cpu) { cpumask_t m = cm_none(); if (cpu < CPU_MAX) m.w[cpu / 64] = 1ULL << (cpu % 64); return m; }
static inline cpumask_t cm_from64(uint64_t bits) { cpumask_t m = cm_none(); m.w[0] = bits; return m; }
static inline int  cm_test(const cpumask_t* m, unsigned cpu) { return cpu < CPU_MAX && ((m->w[cpu / 64] >> (cpu % 64)) & 1); }
static inline void cm_set(cpumask_t* m, unsigned cpu) { if (cpu < CPU_MAX) m->w[cpu / 64] |= 1ULL << (cpu % 64); }
static inline void cm_clear(cpumask_t* m, unsigned cpu) { if (cpu < CPU_MAX) m->w[cpu / 64] &= ~(1ULL << (cpu % 64)); }
static inline int  cm_empty(const cpumask_t* m) { for (int i = 0; i < CPUMASK_WORDS; i++) if (m->w[i]) return 0; return 1; }
static inline int  cm_is64(const cpumask_t* m, uint64_t bits) {          /* для проверок: маска равна bits в первых 64 */
    if (m->w[0] != bits) return 0;
    for (int i = 1; i < CPUMASK_WORDS; i++) if (m->w[i]) return 0;
    return 1;
}
static inline int  cm_is_all(const cpumask_t* m) { for (int i = 0; i < CPUMASK_WORDS; i++) if (m->w[i] != ~0ULL) return 0; return 1; }

#endif
