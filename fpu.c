#include "fpu.h"
#include "cpu.h"
#include "uart.h"

static uint8_t initial_state[FPU_STATE_SIZE] __attribute__((aligned(16)));

static inline uint64_t read_cr0(void) { uint64_t v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr4(void) { uint64_t v; __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr0(uint64_t v) { __asm__ volatile ("mov %0, %%cr0" : : "r"(v) : "memory"); }
static inline void write_cr4(uint64_t v) { __asm__ volatile ("mov %0, %%cr4" : : "r"(v) : "memory"); }

// Регистры управления x87/SSE на текущем процессоре (CR0, CR4, начальное состояние).
void fpu_init_cpu(void) {
    uint64_t cr0 = read_cr0();
    cr0 &= ~(1ULL << 2);
    cr0 |= (1ULL << 1) | (1ULL << 5);
    write_cr0(cr0);
    __asm__ volatile ("clts");
    write_cr4(read_cr4() | (1ULL << 9) | (1ULL << 10));
    static const uint32_t mxcsr_default = 0x1F80;
    __asm__ volatile ("fninit");
    __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr_default));
}

int fpu_init(void) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    int has_fpu = d & 1, has_fxsr = (d >> 24) & 1, has_sse = (d >> 25) & 1, has_sse2 = (d >> 26) & 1;
    if (!has_fpu || !has_fxsr || !has_sse || !has_sse2) {
        uart_print("[KIRA FPU ERROR] CPU lacks x87/FXSR/SSE/SSE2 (required by x86-64).\n");
        return -1;
    }

    // CR0: EM=0 (не эмулировать), MP=1, NE=1 (ошибки x87 через #MF), TS=0 (без ленивого переключения:
    // состояние сохраняется и восстанавливается при каждом переключении потоков).
    uint64_t cr0 = read_cr0();
    cr0 &= ~(1ULL << 2);
    cr0 |= (1ULL << 1) | (1ULL << 5);
    write_cr0(cr0);
    __asm__ volatile ("clts");

    // CR4: OSFXSR (SSE-инструкции и FXSAVE), OSXMMEXCPT (#XM вместо #UD при SIMD-ошибках)
    write_cr4(read_cr4() | (1ULL << 9) | (1ULL << 10));

    static const uint32_t mxcsr_default = 0x1F80; // все SIMD-исключения замаскированы, округление к ближайшему
    __asm__ volatile ("fninit");
    __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr_default));
    fpu_save(initial_state);

    int has_xsave = (c >> 26) & 1, has_avx = (c >> 28) & 1;
    uart_print("[KIRA FPU] x87 + SSE/SSE2 enabled; per-thread FXSAVE state (512 B) saved on every context switch.");
    if (has_avx && has_xsave) uart_print(" AVX present but not enabled (needs XSAVE area).");
    uart_print("\n");
    return 0;
}

const void* fpu_initial_state(void) { return initial_state; }
