/* x86-64: железо под интерпретатор AML (acpidev.c). Здесь - всё, что трогает машину: порты, память, конфигурация PCI, сон и ожидание,
   прерывание SCI и отдельный стек для разбора. Сам AML (aml.c, acpidev.c) железа не знает и общается только с этим файлом через aml_ops_t.

   Что AML трогать нельзя (проверяется здесь, а не доверяется таблице): запись в порты PIC, таймера PIT, CMOS и конфигурации PCI (0xCF8-0xCFF) -
   ими владеет ядро; обращения к ОЗУ, которую ядро выдаёт программам и себе (SystemMemory внутри управляемой памяти, кроме областей ACPI);
   нижние 640 КБ. Такое обращение возвращает ошибку методу и считается в счётчик denied. */
#include "acpirt.h"
#include "../../acpi.h"
#include "../../acpidev.h"
#include "../../uart.h"
#include "../../heap.h"
#include "../../vmm.h"
#include "../../pmm.h"
#include "../../pci.h"
#include "../../clock.h"
#include "../../sched.h"
#include "../../timer.h"
#include "cpu.h"
#include "pic.h"
#include "irqmode.h"
#include "ioapic.h"

#define AML_STACK_BYTES (96u * 1024u)
#define AML_STACK_RESERVE (10u * 1024u)      /* запас под мелкие функции и прерывания на этом стеке */

static inline void outb_(uint16_t p, uint8_t v) { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outw_(uint16_t p, uint16_t v) { __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outl_(uint16_t p, uint32_t v) { __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint8_t inb_(uint16_t p) { uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t inw_(uint16_t p) { uint16_t v; __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t inl_(uint16_t p) { uint32_t v; __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

static struct {
    int up;                        /* интерпретатор работает */
    uint8_t* stack;                /* отдельный стек для разбора */
    volatile int busy;
    unsigned sci_irq; int sci_wired;
    volatile int sci_masked;
    volatile uint32_t sci_count; uint64_t sci_win_start; uint32_t sci_win_n; int sci_storm;
    volatile int poweroff_req, sleep_req;
    uint32_t denied, mem_ok, io_ok;
    pmm_range_t acpi_rng[16]; unsigned n_acpi; pmm_range_t high[PMM_MAX_HIGH]; unsigned n_high;
} R;

/* ---------------- отдельный стек ---------------- */

/* fn(arg) выполняется на стеке R.stack. Прерывания остаются включёнными (на этом же стеке они и обрабатываются); планировщик может усыпить
   вызывающего посреди AML (Sleep) - указатель стека у потока сохраняется вместе с остальным. Одновременно - только один вызов. */
static void run_on_stack(void (*fn)(void*), void* arg) {
    if (__atomic_exchange_n(&R.busy, 1, __ATOMIC_ACQUIRE)) { uart_print("[KIRA ACPI] AML is busy: call skipped.\n"); return; }
    uint64_t top = ((uint64_t)(uintptr_t)R.stack + AML_STACK_BYTES) & ~0xFull;
    __asm__ volatile (
        "mov %%rsp, %%r12\n\t"
        "mov %[top], %%rsp\n\t"
        "call *%[fn]\n\t"
        "mov %%r12, %%rsp\n\t"
        : : [top] "r"(top), [fn] "r"(fn), "D"(arg)
        : "r12", "rax", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11", "memory", "cc");
    __atomic_store_n(&R.busy, 0, __ATOMIC_RELEASE);
}

/* ---------------- проверка доступа ---------------- */

static int ranges_hit(const pmm_range_t* r, unsigned n, uint64_t a, uint64_t b) {
    for (unsigned i = 0; i < n; i++) if (a < r[i].end && b > r[i].start) return 1;
    return 0;
}
static int ranges_contain(const pmm_range_t* r, unsigned n, uint64_t a, uint64_t b) {
    for (unsigned i = 0; i < n; i++) if (a >= r[i].start && b <= r[i].end) return 1;
    return 0;
}
static int mem_allowed(uint64_t a, unsigned nb) {
    uint64_t b = a + nb;
    if (b < a) return 0;
    if (b <= 0xA0000) return 0;                                          /* низкие 640 КБ: данные BIOS, наш код для запуска ядер */
    if (a >= 0x100000 && a < pmm_managed_top()) return ranges_contain(R.acpi_rng, R.n_acpi, a, b);   /* ОЗУ ядра - только области ACPI */
    if (b > 0x100000 && a < 0x100000) return 0;                          /* спан границы 1 МБ */
    if (ranges_hit(R.high, R.n_high, a, b)) return 0;                    /* ОЗУ выше 4 ГБ */
    return 1;
}
static int io_write_allowed(uint64_t p) {
    if ((p >= 0x20 && p <= 0x21) || (p >= 0xA0 && p <= 0xA1)) return 0;   /* PIC */
    if (p >= 0x40 && p <= 0x43) return 0;                                 /* PIT */
    if (p >= 0x70 && p <= 0x71) return 0;                                 /* CMOS/RTC: ядро читает часы */
    if (p >= 0xCF8 && p <= 0xCFF) return 0;                               /* конфигурация PCI через порты: у ядра своя блокировка */
    return 1;
}

static int rt_rw(void* u, int write, unsigned sp, uint64_t addr, unsigned bits, uint64_t* val, uint32_t pci) {
    (void)u;
    unsigned nb = bits / 8;
    if (bits != 8 && bits != 16 && bits != 32 && bits != 64) return -1;
    switch (sp) {
    case AML_SP_IO:
        if (addr > 0xFFFF || addr + nb > 0x10000 || bits == 64) return -1;
        if (write && !io_write_allowed(addr)) { R.denied++; return -1; }
        R.io_ok++;
        if (write) { if (bits == 8) outb_((uint16_t)addr, (uint8_t)*val); else if (bits == 16) outw_((uint16_t)addr, (uint16_t)*val); else outl_((uint16_t)addr, (uint32_t)*val); }
        else { if (bits == 8) *val = inb_((uint16_t)addr); else if (bits == 16) *val = inw_((uint16_t)addr); else *val = inl_((uint16_t)addr); }
        return 0;
    case AML_SP_MEM: {
        if (!mem_allowed(addr, nb)) { R.denied++; return -1; }
        volatile uint8_t* p;
        if (addr + nb <= (1ull << 30)) p = (volatile uint8_t*)(uintptr_t)addr;        /* первый гигабайт - тождественная карта ядра */
        else {
            uint64_t page = addr & ~0xFFFull;
            volatile uint8_t* base = (volatile uint8_t*)vmm_map_mmio(page, 0x2000, VMM_MMIO_UC);
            if (!base) return -1;
            p = base + (addr - page);
        }
        R.mem_ok++;
        if (write) { if (bits == 8) *p = (uint8_t)*val; else if (bits == 16) *(volatile uint16_t*)p = (uint16_t)*val; else if (bits == 32) *(volatile uint32_t*)p = (uint32_t)*val; else *(volatile uint64_t*)p = *val; }
        else { if (bits == 8) *val = *p; else if (bits == 16) *val = *(volatile uint16_t*)p; else if (bits == 32) *val = *(volatile uint32_t*)p; else *val = *(volatile uint64_t*)p; }
        return 0; }
    case AML_SP_PCI: {
        if ((pci >> 16) != 0 || bits == 64 || addr + nb > 256) return -1;          /* только сегмент 0 и стандартные 256 байт */
        pci_dev_t d; unsigned i;
        for (i = 0; i < sizeof d; i++) ((uint8_t*)&d)[i] = 0;
        d.bus = (uint8_t)(pci >> 8); d.dev = (uint8_t)((pci >> 3) & 31); d.fn = (uint8_t)(pci & 7);
        uint16_t reg = (uint16_t)(addr & ~3ull); unsigned sh = (unsigned)(addr & 3) * 8;
        if (sh + bits > 32) return -1;                                             /* обращение через границу слова */
        if (write && reg < 0x40) { R.denied++; return -1; }                        /* заголовок (BAR, команда) настраивает только ядро */
        uint32_t w = pci_read32(&d, reg);
        uint32_t mask = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u);
        if (write) { w = (w & ~(mask << sh)) | (((uint32_t)*val & mask) << sh); pci_write32(&d, reg, w); }
        else *val = (w >> sh) & mask;
        return 0; }
    default:
        return -1;
    }
}

static void rt_stall(void* u, uint32_t us) {
    (void)u;
    uint64_t khz = clock_tsc_khz();
    if (!khz) { for (uint32_t i = 0; i < us; i++) outb_(0x80, 0); return; }
    uint64_t end = rdtsc() + khz * us / 1000;
    while (rdtsc() < end) cpu_relax();
}
static void rt_sleep(void* u, uint32_t ms) {
    (void)u;
    if (timer_get_ticks() == 0 || ms < 2) { rt_stall(0, ms * 1000u); return; }
    uint32_t t = (uint32_t)((uint64_t)ms * TIMER_HZ / 1000);
    sched_sleep_ticks(t ? t : 1);
}
static uint64_t rt_timer(void* u) { (void)u; return clock_ns() / 100; }
static void rt_log(void* u, const char* s) { (void)u; uart_print("[KIRA ACPI] AML: "); uart_print(s); uart_print("\n"); }

/* ---------------- SCI ---------------- */

static void sci_mask(int on_off_mask) {
    if (R.sci_wired) {
        if (irqmode_is_apic()) (void)irqmode_pci_line(R.sci_irq, on_off_mask ? 0 : 1);
        else if (on_off_mask) pic_mask_irq((unsigned char)R.sci_irq); else pic_unmask_irq((unsigned char)R.sci_irq);
    }
}

/* В прерывании - только отметка и маска линии: уровневый SCI не уйдёт, пока не сброшен источник, а сброс - работа для основного цикла. */
static void sci_handler(void) {
    acpidev_sci();
    R.sci_count++;
    uint64_t now = clock_ns();
    if (now - R.sci_win_start > 1000000000ull) { R.sci_win_start = now; R.sci_win_n = 0; }
    if (++R.sci_win_n > 2000) R.sci_storm = 1;               /* шторм: линию больше не включаем, события разбираются опросом раз в 250 мс */
    sci_mask(1);
    R.sci_masked = 1;
}

/* ---------------- события для ядра ---------------- */

static void on_event(int ev, int arg, void* u) {
    (void)u;
    switch (ev) {
    case ACPIDEV_EV_POWER_BUTTON: uart_print("[KIRA ACPI] power button pressed: shutting down cleanly.\n"); R.poweroff_req = 1; break;
    case ACPIDEV_EV_SLEEP_BUTTON: uart_print("[KIRA ACPI] sleep button pressed: going to sleep (S3).\n"); R.sleep_req = 1; break;       /* кнопка питания не затронута: она по-прежнему выключает */
    case ACPIDEV_EV_LID: uart_print(arg ? "[KIRA ACPI] lid opened.\n" : "[KIRA ACPI] lid closed: going to sleep (S3).\n"); if (!arg) R.sleep_req = 1; break;
    default: break;
    }
}

int acpirt_take_poweroff(void) { return __atomic_exchange_n(&R.poweroff_req, 0, __ATOMIC_ACQ_REL); }

/* ---------------- запуск ---------------- */

static void sink_print(const char* s) { uart_print(s); uart_print("\n"); }
int acpirt_up(void) { return R.up; }
void acpirt_dump(void) {
    acpidev_dump(sink_print);
    uart_print("ACPI платформа: SCI IRQ "); uart_print_dec(R.sci_irq);
    uart_print(R.sci_wired ? (R.sci_storm ? " (шторм: только опрос)" : " (прерывание)") : " (только опрос раз в 250 мс)");
    uart_print(", прерываний "); uart_print_dec(R.sci_count); uart_print(", доступов: порты "); uart_print_dec(R.io_ok); uart_print(", память "); uart_print_dec(R.mem_ok);
    uart_print(", запрещено "); uart_print_dec(R.denied); uart_print("\n");
}

int acpirt_init(void) {
    const acpi_info_t* a = acpi_get();
    if (!a || !a->dsdt_phys) { uart_print("[KIRA ACPI] no DSDT: AML interpreter is not started.\n"); return -1; }
    acpidev_tab_t dsdt = { acpi_table_view(a->dsdt_phys, a->dsdt_len), a->dsdt_len };
    acpidev_tab_t ssdt[ACPI_MAX_SSDT]; unsigned ns = 0;
    for (unsigned i = 0; i < a->ssdt_count && i < ACPI_MAX_SSDT; i++) {
        const uint8_t* p = acpi_table_view(a->ssdt_phys[i], a->ssdt_len[i]);
        if (p) { ssdt[ns].p = p; ssdt[ns].len = a->ssdt_len[i]; ns++; }
    }
    if (!dsdt.p) { uart_print("[KIRA ACPI] DSDT is not readable.\n"); return -2; }
    size_t sz = acpidev_arena_size(&dsdt, ssdt, ns);
    void* mem = kmalloc(sz);
    R.stack = (uint8_t*)kmalloc(AML_STACK_BYTES);
    if (!mem || !R.stack) { uart_print("[KIRA ACPI] no memory for the AML interpreter.\n"); if (mem) kfree(mem); if (R.stack) kfree(R.stack); R.stack = 0; return -3; }
    R.n_acpi = pmm_acpi_ranges(R.acpi_rng, 16);
    R.n_high = pmm_high_ranges(R.high, PMM_MAX_HIGH);

    /* Режим ACPI: SCI_EN. Без него события не приходят (так же, как перед выключением). */
    if (a->pm1a_cnt && a->pm1a_cnt <= 0xFFFF && !(inw_((uint16_t)a->pm1a_cnt) & 1) && a->smi_cmd && a->acpi_enable) {
        outb_((uint16_t)a->smi_cmd, a->acpi_enable);
        for (int i = 0; i < 3000000 && !(inw_((uint16_t)a->pm1a_cnt) & 1); i++) cpu_relax();
    }

    aml_ops_t ops = { rt_rw, rt_sleep, rt_stall, rt_timer, 0, rt_log, 0 };
    acpidev_hw_t hw;
    hw.sci_int = a->sci_int; hw.pm1a_evt = a->pm1a_evt; hw.pm1b_evt = a->pm1b_evt; hw.pm1_evt_len = a->pm1_evt_len;
    hw.gpe0_blk = a->gpe0_blk; hw.gpe1_blk = a->gpe1_blk; hw.gpe0_len = a->gpe0_len; hw.gpe1_len = a->gpe1_len; hw.gpe1_base = a->gpe1_base; hw.fadt_flags = a->fadt_flags;
    acpidev_set_runner(run_on_stack, R.stack + AML_STACK_RESERVE);
    int rc = acpidev_init(mem, sz, &ops, &hw, &dsdt, ssdt, ns);
    if (rc != 0) {
        uart_print("[KIRA ACPI] AML interpreter failed to start, code "); uart_print_dec((uint64_t)(-rc)); uart_print(" (shutdown by \\_S5 scan keeps working).\n");
        kfree(mem); kfree(R.stack); R.stack = 0;
        return rc;
    }
    R.up = 1;
    acpidev_set_hook(on_event, 0);
    /* Прошивке - режим контроллера прерываний, потом события. */
    (void)acpidev_pic_mode(irqmode_is_apic());
    R.sci_irq = a->sci_int;
    if (R.sci_irq >= 3 && R.sci_irq < 16 && !irq_has_handler(R.sci_irq) && irq_register_handler((unsigned char)R.sci_irq, sci_handler) == 0) {
        R.sci_wired = 1;
        R.sci_win_start = clock_ns();
        if (irqmode_is_apic()) { if (irqmode_pci_line(R.sci_irq, 1) != 0) { R.sci_wired = 0; irq_unregister_handler(R.sci_irq); } }
        else pic_unmask_irq((unsigned char)R.sci_irq);
    }
    (void)acpidev_enable_events();
    aml_stats_t st; aml_stats(acpidev_ctx(), &st);
    const acpidev_power_t* w = acpidev_power();
    uart_print("[KIRA ACPI] AML up: "); uart_print_dec(st.nodes); uart_print(" objects, arena "); uart_print_dec(st.arena_used / 1024); uart_print(" KB of ");
    uart_print_dec(st.arena_size / 1024); uart_print(" KB, SCI IRQ "); uart_print_dec(R.sci_irq); uart_print(R.sci_wired ? " (interrupt)" : " (polled)");
    uart_print(", batteries "); uart_print_dec((uint64_t)w->batteries); uart_print("\n");
    return 0;
}

void acpirt_poll(void) {
    if (!R.up) return;
    acpidev_poll(clock_ns() / 1000000ull);
    if (R.sci_masked && !R.sci_storm) { R.sci_masked = 0; sci_mask(0); }
}

/* ---------------- сон S3 ---------------- */
int acpirt_sx(int state, uint8_t* a, uint8_t* b) { return R.up ? acpidev_sx(state, a, b) : -1; }
void acpirt_wake(int state) { if (R.up) (void)acpidev_wake(state); }
int acpirt_arm_wake(int state, unsigned* count) { *count = 0; return R.up ? acpidev_arm_wake(state, count) : -1; }
int acpirt_take_sleep(void) { return __atomic_exchange_n(&R.sleep_req, 0, __ATOMIC_ACQ_REL); }
/* После пробуждения контроллер событий ACPI в состоянии сброса: прошивке сообщается режим контроллера прерываний, события и GPE включаются заново,
   линия SCI размаскируется (шторм и счётчики сбрасываются: до сна они не имеют значения). */
void acpirt_resume(void) {
    if (!R.up) return;
    R.sci_storm = 0; R.sci_win_n = 0; R.sci_win_start = clock_ns();
    (void)acpidev_pic_mode(irqmode_is_apic());
    (void)acpidev_enable_events();
    if (R.sci_wired) { R.sci_masked = 0; sci_mask(0); }
}
int acpirt_s5(uint8_t* a, uint8_t* b) { return R.up ? acpidev_s5(a, b) : -1; }
void acpirt_prepare_sleep(int state) { if (R.up) (void)acpidev_prepare_sleep(state); }
