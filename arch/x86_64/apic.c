#include "apic.h"
#include "cpu.h"
#include "idt.h"
#include "uart.h"
#include "vmm.h"

#define MSR_APIC_BASE      0x1B
#define APIC_BASE_ENABLE   (1ULL << 11)
#define APIC_BASE_X2APIC   (1ULL << 10)
#define APIC_BASE_MASK     0x000FFFFFFFFFF000ULL
#define MSR_X2APIC_BASE    0x800             /* регистр xAPIC со смещением R = MSR 0x800 + R/16 */
#define MSR_X2APIC_ICR     0x830

#define APIC_REG_ID        0x020
#define APIC_REG_VERSION   0x030
#define APIC_REG_EOI       0x0B0
#define APIC_REG_SVR       0x0F0
#define APIC_REG_ICR_LOW   0x300
#define APIC_REG_ICR_HIGH  0x310
#define APIC_ICR_BUSY      (1u << 12)
#define APIC_REG_LVT_TIMER 0x320
#define APIC_REG_TICR      0x380
#define APIC_REG_TCCR      0x390
#define APIC_REG_TDCR      0x3E0

#define APIC_SVR_ENABLE    0x100
#define APIC_LVT_MASKED    (1u << 16)
#define APIC_LVT_PERIODIC  (1u << 17)
#define APIC_TDCR_DIV16    0x3

// Два режима одного устройства: xAPIC (регистры в MMIO, ID 8 бит) и x2APIC (регистры - MSR, ID 32 бита, ICR одной
// записью). x2APIC обязателен, если его уже включила прошивка (выйти из него можно только через сброс), и нужен
// машинам с APIC ID больше 254. Режим общий для всех ядер: остальные ядра включают тот же.
static volatile uint32_t* apic_regs;
static int x2;
static int enabled;

static void apic_write(uint32_t reg, uint32_t value) {
    if (x2) cpu_wrmsr(MSR_X2APIC_BASE + (reg >> 4), value);
    else apic_regs[reg / 4] = value;
}

static uint32_t apic_read(uint32_t reg) {
    if (x2) return (uint32_t)cpu_rdmsr(MSR_X2APIC_BASE + (reg >> 4));
    return apic_regs[reg / 4];
}

int apic_is_x2(void) { return x2; }

int apic_init(int want_x2, uint32_t max_apic_id) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (!(d & (1u << 9))) {
        uart_print("[KIRA APIC] CPUID: no Local APIC.\n");
        return -1;
    }
    int can_x2 = (c >> 21) & 1;
    uint64_t msr = cpu_rdmsr(MSR_APIC_BASE);
    const char* why = 0;
    if (msr & APIC_BASE_X2APIC) why = "firmware enabled it";
    else if (can_x2 && max_apic_id > 254) why = "APIC IDs above 254";
    else if (can_x2 && want_x2) why = "boot option x2apic";
    else if (want_x2) uart_print("[KIRA APIC] Boot option x2apic: this CPU has no x2APIC, using xAPIC.\n");

    if (why) {
        x2 = 1;
        if (!(msr & APIC_BASE_ENABLE)) { msr |= APIC_BASE_ENABLE; cpu_wrmsr(MSR_APIC_BASE, msr); }
        if (!(msr & APIC_BASE_X2APIC)) cpu_wrmsr(MSR_APIC_BASE, msr | APIC_BASE_X2APIC);   /* xAPIC -> x2APIC: только при включённом */
    } else {
        if (max_apic_id > 254) uart_print("[KIRA APIC] APIC IDs above 254 but no x2APIC: those processors cannot be addressed.\n");
        if (!(msr & APIC_BASE_ENABLE)) cpu_wrmsr(MSR_APIC_BASE, msr | APIC_BASE_ENABLE);
        apic_regs = (volatile uint32_t*)vmm_map_mmio(msr & APIC_BASE_MASK, 4096, VMM_MMIO_UC);
        if (!apic_regs) {
            uart_print("[KIRA APIC] Failed to map LAPIC MMIO page.\n");
            return -1;
        }
    }

    apic_write(APIC_REG_SVR, APIC_SVR_ENABLE | APIC_SPURIOUS_VECTOR);
    if (!(apic_read(APIC_REG_SVR) & APIC_SVR_ENABLE)) {
        uart_print("[KIRA APIC] LAPIC refused software enable.\n");
        return -1;
    }
    enabled = 1;

    if (x2) { uart_print("[KIRA APIC] x2APIC mode ("); uart_print(why); uart_print("): registers via MSR, 32-bit IDs, id "); }
    else { uart_print("[KIRA APIC] xAPIC mode: registers in the kernel MMIO window, base "); uart_print_hex(msr & APIC_BASE_MASK); uart_print(", id "); }
    uart_print_dec(apic_get_id());
    uart_print(", version ");
    uart_print_hex(apic_get_version());
    uart_print("\n");
    return 0;
}

int apic_is_enabled(void) { return enabled; }

uint32_t apic_get_id(void) {
    if (!enabled) return 0;
    return x2 ? apic_read(APIC_REG_ID) : (apic_read(APIC_REG_ID) >> 24);
}

uint32_t apic_get_version(void) {
    return enabled ? (apic_read(APIC_REG_VERSION) & 0xFF) : 0;
}

void apic_send_eoi(void) {
    if (enabled) apic_write(APIC_REG_EOI, 0);
}

static uint32_t timer_initial;       /* начальный счёт, которым запущен таймер: остальные ядра берут тот же */

uint32_t apic_timer_initial_count(void) { return timer_initial; }

void apic_timer_start_periodic(uint8_t vector, uint32_t initial_count) {
    if (!enabled || initial_count == 0) return;
    timer_initial = initial_count;
    apic_write(APIC_REG_TDCR, APIC_TDCR_DIV16);
    apic_write(APIC_REG_LVT_TIMER, (uint32_t)vector | APIC_LVT_PERIODIC);
    apic_write(APIC_REG_TICR, initial_count);
}

// Останавливает таймер только этого ядра (общее значение счёта сохраняется для запуска других ядер).
void apic_timer_mask_local(void) {
    if (!enabled) return;
    apic_write(APIC_REG_LVT_TIMER, APIC_LVT_MASKED | APIC_SPURIOUS_VECTOR);
    apic_write(APIC_REG_TICR, 0);
}

void apic_timer_stop(void) {
    if (!enabled) return;
    timer_initial = 0;
    apic_timer_mask_local();
}

// Частота счётчика таймера LAPIC (делитель 16) по уже откалиброванному TSC: 20 мс ожидания по TSC дают точность
// порядка сотой доли процента (вместо 2% при отсчёте по тикам PIT).
uint64_t apic_calibrate_timer_tsc(uint64_t tsc_khz) {
    if (!enabled || tsc_khz == 0) return 0;
    apic_write(APIC_REG_TDCR, APIC_TDCR_DIV16);
    apic_write(APIC_REG_LVT_TIMER, APIC_LVT_MASKED | APIC_SPURIOUS_VECTOR);   // без прерываний
    uint64_t f = irq_save();
    uint64_t span = tsc_khz * 20;                                            // 20 мс в тактах TSC
    apic_write(APIC_REG_TICR, 0xFFFFFFFFu);
    uint64_t t0 = rdtsc();
    while (rdtsc() - t0 < span) __asm__ volatile ("pause");
    uint32_t remaining = apic_read(APIC_REG_TCCR);
    uint64_t t1 = rdtsc();
    apic_write(APIC_REG_TICR, 0);
    irq_restore(f);
    uint64_t counted = 0xFFFFFFFFull - remaining;
    return counted * tsc_khz * 1000 / (t1 - t0);
}

int apic_send_ipi(uint32_t dest_apic_id, uint32_t icr_low) {
    if (!enabled) return -1;
    if (x2) {
        // x2APIC: одна запись в MSR ICR (адресат в старших 32 битах), бита занятости нет. Снятие INIT по уровню
        // в x2APIC не поддерживается и не нужно.
        if ((icr_low & 0xC700u) == 0x8500u) return 0;
        cpu_wrmsr(MSR_X2APIC_ICR, ((uint64_t)dest_apic_id << 32) | icr_low);
        return 0;
    }
    uint64_t f = irq_save();                   // ICR_HIGH и ICR_LOW пишутся парой: между ними нельзя вклиниться
    for (int i = 0; i < 1000000 && (apic_read(APIC_REG_ICR_LOW) & APIC_ICR_BUSY); i++) __asm__ volatile ("pause");
    apic_write(APIC_REG_ICR_HIGH, dest_apic_id << 24);
    apic_write(APIC_REG_ICR_LOW, icr_low);     // запись младшей половины запускает отправку
    int rc = -1;
    for (int i = 0; i < 1000000; i++) {
        if (!(apic_read(APIC_REG_ICR_LOW) & APIC_ICR_BUSY)) { rc = 0; break; }
        __asm__ volatile ("pause");
    }
    irq_restore(f);
    return rc;
}

int apic_enable_this_cpu(void) {
    if (!enabled) return -1;
    uint64_t msr = cpu_rdmsr(MSR_APIC_BASE);
    if (!(msr & APIC_BASE_ENABLE)) { msr |= APIC_BASE_ENABLE; cpu_wrmsr(MSR_APIC_BASE, msr); }
    if (x2 && !(msr & APIC_BASE_X2APIC)) cpu_wrmsr(MSR_APIC_BASE, msr | APIC_BASE_X2APIC);
    apic_write(APIC_REG_SVR, APIC_SVR_ENABLE | APIC_SPURIOUS_VECTOR);
    return (apic_read(APIC_REG_SVR) & APIC_SVR_ENABLE) ? 0 : -1;
}
