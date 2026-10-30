#include "irqmode.h"
#include "acpi.h"
#include "apic.h"
#include "ioapic.h"
#include "pic.h"
#include "timer.h"
#include "keyboard.h"
#include "cpu.h"
#include "uart.h"

#define CMOS_IDX  0x70
#define CMOS_DATA 0x71

static int apic_mode;
static volatile uint32_t irq_target;
static volatile uint32_t rtc_hits;

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static uint8_t cmos_rd(uint8_t r) { outb(CMOS_IDX, r); return inb(CMOS_DATA); }
static void cmos_wr(uint8_t r, uint8_t v) { outb(CMOS_IDX, r); outb(CMOS_DATA, v); }

static void rtc_test_handler(void) {
    (void)cmos_rd(0x0C);          /* без чтения регистра C RTC не пришлёт следующее прерывание */
    rtc_hits++;
}

static void spin_ms(uint64_t khz, uint32_t ms) {
    uint64_t end = rdtsc() + khz * ms;
    while (rdtsc() < end) __asm__ volatile ("pause");
}

int irqmode_is_apic(void) { return apic_mode; }
uint32_t irqmode_target(void) { return irq_target; }

int irqmode_core_failed(uint32_t failed_apic_id, uint32_t new_apic_id) {
    if (!apic_mode) return -1;
    if (irq_target != failed_apic_id) return 0;
    const acpi_info_t* a = acpi_get();
    if (!a) return -1;
    int moved = 0;
    for (unsigned irq = 1; irq < 16; irq++) {
        if (irq == 2 || !irq_has_handler(irq)) continue;
        if (ioapic_route_isa(a, irq, new_apic_id) == 0) moved++;
    }
    irq_target = new_apic_id;
    keyboard_handler();            /* байт, застрявший в 8042 вместе с потерянным на этом ядре прерыванием, забираем сами */
    return moved;
}

/* Сквозная проверка IOAPIC: настоящее устройство (RTC, 1024 Гц) -> IOAPIC -> LAPIC -> наш обработчик. */
static int test_ioapic_delivery(const acpi_info_t* a, uint64_t khz, uint32_t dest) {
    if (irq_register_handler(8, rtc_test_handler) != 0) return -1;
    irq_set_eoi_apic(8, 1);
    rtc_hits = 0;
    int rc = ioapic_route_isa(a, 8, dest);
    if (rc != 0) { irq_set_eoi_apic(8, 0); irq_unregister_handler(8); return -2; }

    uint8_t ra = cmos_rd(0x0A), rb = cmos_rd(0x0B);
    cmos_wr(0x0A, (uint8_t)((ra & 0xF0) | 6));     /* 1024 Гц */
    cmos_wr(0x0B, (uint8_t)(rb | 0x40));           /* PIE: периодическое прерывание */
    (void)cmos_rd(0x0C);
    spin_ms(khz, 50);
    cmos_wr(0x0B, (uint8_t)(rb & ~0x40));
    ioapic_mask_isa(a, 8, 1);
    (void)cmos_rd(0x0C);
    cmos_wr(0x0A, ra);
    irq_set_eoi_apic(8, 0);
    irq_unregister_handler(8);
    return rtc_hits >= 20 ? 0 : -3;                /* ожидаем ~51 прерывание за 50 мс */
}

int irqmode_enable_apic(uint64_t khz, uint64_t rate, int have_lapic, uint32_t tick_hz) {
    if (!have_lapic) { uart_print("[KIRA IRQ] No Local APIC: staying on PIC + PIT.\n"); return -1; }
    const acpi_info_t* a = acpi_get();
    if (!a || a->ioapic_count == 0) { uart_print("[KIRA IRQ] ACPI lists no IOAPIC: staying on PIC + PIT.\n"); return -1; }
    if (khz == 0 || rate == 0 || tick_hz == 0) { uart_print("[KIRA IRQ] Clock calibration missing: staying on PIC + PIT.\n"); return -1; }
    if (ioapic_init(a) != 0) { uart_print("[KIRA IRQ] IOAPIC check failed: staying on PIC + PIT.\n"); return -2; }
    uint32_t dest = apic_get_id();
    irq_target = dest;

    int rc = test_ioapic_delivery(a, khz, dest);
    if (rc != 0) {
        uart_print("[KIRA IRQ] IOAPIC delivery test FAILED (code "); uart_print_dec((uint64_t)(-rc));
        uart_print(", "); uart_print_dec(rtc_hits); uart_print(" RTC interrupts): staying on PIC + PIT.\n");
        return -3;
    }
    uart_print("[KIRA IRQ] IOAPIC delivery verified end-to-end: RTC -> IOAPIC -> LAPIC, ");
    uart_print_dec(rtc_hits); uart_print(" interrupts in 50 ms.\n");

    /* Таймер LAPIC на вектор 32 (линия 0). PIT при этом снимаем с PIC, чтобы тики не удваивались. */
    irq_set_eoi_apic(0, 1);
    pic_mask_irq(0);
    apic_timer_start_periodic(32, (uint32_t)(rate / tick_hz));
    uint64_t want = tick_hz / 5;                   /* за 200 мс */
    uint64_t got = 0;
    /* Нижняя граница мягче верхней: нагруженный эмулятор теряет часть прерываний, но таймер при этом рабочий.
       До трёх замеров: на перегруженной машине-хозяине процессор эмулятора может надолго не получить время,
       и один провальный замер ещё не значит, что таймер неисправен. */
    for (int attempt = 1; attempt <= 3; attempt++) {
        uint64_t t0 = timer_get_irqs();
        spin_ms(khz, 200);
        got = timer_get_irqs() - t0;               /* именно прерывания: тики догоняются по TSC и ничего бы не доказали */
        if (got >= want / 2 && got <= want + want / 4) break;
        uart_print("[KIRA IRQ] LAPIC timer check "); uart_print_dec((uint64_t)attempt);
        uart_print(": "); uart_print_dec(got); uart_print(" ticks in 200 ms, expected "); uart_print_dec(want);
        uart_print(attempt < 3 ? ", measuring again.\n" : ".\n");
    }
    if (got < want / 2 || got > want + want / 4) {
        apic_timer_stop();
        irq_set_eoi_apic(0, 0);
        pic_unmask_irq(0);
        uart_print("[KIRA IRQ] LAPIC timer test FAILED ("); uart_print_dec(got);
        uart_print(" ticks in 200 ms, expected "); uart_print_dec(want);
        uart_print("): back on PIT.\n");
        return -4;
    }
    uart_print("[KIRA IRQ] LAPIC timer verified: "); uart_print_dec(got);
    uart_print(" ticks in 200 ms (expected "); uart_print_dec(want); uart_print(").\n");

    /* Фиксируем: маршрутизируем зарегистрированные линии через IOAPIC, PIC отключаем. */
    uint64_t f = irq_save();
    unsigned routed = 0;
    for (unsigned irq = 1; irq < 16; irq++) {
        if (irq == 2 || !irq_has_handler(irq)) continue;
        if (ioapic_route_isa(a, irq, dest) == 0) routed++;
        else { uart_print("[KIRA IRQ] cannot route IRQ "); uart_print_dec(irq); uart_print(" via IOAPIC.\n"); }
    }
    irq_set_eoi_apic_all();
    pic_disable();
    keyboard_handler();                            /* слить возможный незабранный байт контроллера 8042 */
    apic_mode = 1;
    irq_restore(f);
    uart_print("[KIRA IRQ] APIC mode: LAPIC timer "); uart_print_dec(tick_hz);
    uart_print(" Hz is the tick source, "); uart_print_dec(routed);
    uart_print(" device lines routed through IOAPIC, legacy PIC and PIT disabled.\n");
    return 0;
}
