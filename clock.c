#include "clock.h"
#include "cpu.h"

static uint64_t base_tsc;
static uint64_t khz;

// cycles * 1e6 / khz без переполнения 64 бит: делим на целую и дробную части.
uint64_t clock_tsc_to_ns(uint64_t cycles, uint64_t tsc_khz) {
    if (tsc_khz == 0) return 0;
    uint64_t whole = cycles / tsc_khz;          // миллисекунды
    uint64_t rem = cycles % tsc_khz;            // < tsc_khz (~1e7), rem * 1e6 < 2^64
    return whole * 1000000ULL + rem * 1000000ULL / tsc_khz;
}

void clock_init(uint64_t tsc_khz) {
    khz = tsc_khz;
    base_tsc = rdtsc();
}

int clock_ready(void) { return khz != 0; }
uint64_t clock_tsc_khz(void) { return khz; }

uint64_t clock_ns(void) {
    if (!khz) return 0;
    uint64_t t = rdtsc();
    if (t < base_tsc) return 0;            /* счётчик этого ядра отстаёт от загрузочного: не даём разности перевернуться */
    return clock_tsc_to_ns(t - base_tsc, khz);
}

uint64_t clock_ms(void) { return clock_ns() / 1000000ULL; }

// ---- Калибровка частоты TSC ----
// Чистая часть: из листа CPUID 0x15 (TSC/кристалл) - точная частота, 0 - лист не даёт ответа.
uint64_t clock_khz_from_cpuid15(uint32_t eax_den, uint32_t ebx_num, uint32_t ecx_hz) {
    if (!eax_den || !ebx_num || !ecx_hz) return 0;
    return (uint64_t)ecx_hz * ebx_num / eax_den / 1000;
}

#ifndef KIRA_HOST_TEST
static inline void outb_(uint16_t p, uint8_t v) { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint8_t inb_(uint16_t p) { uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }

#define PIT_HZ    1193182ULL
#define PIT_LATCH 59659u                 /* 50 мс */

// Канал 2 PIT в режиме 0 (однократный счёт), выход OUT2 читается в порту 0x61: интервал 50 мс измеряется по TSC
// с точностью до одного чтения порта (около микросекунды), то есть ~20 ppm. Не нужен ни тик, ни прерывания.
static uint64_t pit2_once(void) {
    uint8_t p61 = inb_(0x61);
    outb_(0x61, (uint8_t)((p61 & ~0x02) | 0x01));            /* вход GATE2 = 1, динамик выключен */
    outb_(0x43, 0xB0);                                        /* канал 2, младший+старший байт, режим 0, двоичный */
    outb_(0x42, PIT_LATCH & 0xFF);
    outb_(0x42, PIT_LATCH >> 8);
    uint64_t t0 = rdtsc();
    if (inb_(0x61) & 0x20) { outb_(0x61, p61); return 0; }   /* OUT2 обязан упасть при записи счёта: иначе канала 2 нет */
    uint32_t guard = 0;
    while (!(inb_(0x61) & 0x20)) {                            /* OUT2 поднимется по окончании счёта */
        if (++guard > 2000000u) { outb_(0x61, p61); return 0; }   /* за 50 мс столько чтений порта не бывает: PIT не идёт */
    }
    uint64_t t1 = rdtsc();
    outb_(0x61, p61);
    return t1 - t0;
}

uint64_t clock_calibrate(const char** method) {
    uint64_t d[3];
    int ok = 1;
    for (int i = 0; i < 3 && ok; i++) { d[i] = pit2_once(); ok = d[i] != 0; }
    uint64_t pit_khz = 0;
    if (ok) {                                                 /* медиана трёх: одно прерывание или SMI не испортит результат */
        uint64_t a = d[0], b = d[1], c = d[2], m;
        m = (a > b) ? ((b > c) ? b : (a > c ? c : a)) : ((a > c) ? a : (b > c ? c : b));
        pit_khz = m * PIT_HZ / PIT_LATCH / 1000;
        if (pit_khz < 100000 || pit_khz > 20000000) pit_khz = 0;   /* вне 100 МГц..20 ГГц - замер испорчен */
    }
    uint32_t a, b, c, dd;
    uint64_t cpuid_khz = 0, base_khz = 0;
    cpuid(0, &a, &b, &c, &dd);
    uint32_t maxleaf = a;
    if (maxleaf >= 0x15) { cpuid(0x15, &a, &b, &c, &dd); cpuid_khz = clock_khz_from_cpuid15(a, b, c); }
    if (maxleaf >= 0x16) { cpuid(0x16, &a, &b, &c, &dd); base_khz = (uint64_t)(a & 0xFFFF) * 1000; }

    if (pit_khz && cpuid_khz) {                               /* оба есть: точный CPUID, если измерение с ним согласно */
        uint64_t diff = pit_khz > cpuid_khz ? pit_khz - cpuid_khz : cpuid_khz - pit_khz;
        if (diff * 100 <= cpuid_khz) { *method = "CPUID 0x15 (crystal ratio), confirmed by PIT"; return cpuid_khz; }
        *method = "PIT channel 2 (CPUID 0x15 disagrees, ignored)";
        return pit_khz;
    }
    if (pit_khz)   { *method = "PIT channel 2, 3x50 ms median"; return pit_khz; }
    if (cpuid_khz) { *method = "CPUID 0x15 (crystal ratio)"; return cpuid_khz; }
    if (base_khz)  { *method = "CPUID 0x16 base frequency (approximate)"; return base_khz; }
    *method = "none";
    return 0;
}
#endif
