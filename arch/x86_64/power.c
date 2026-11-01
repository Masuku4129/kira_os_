/* x86-64: выключение (ACPI S5: SLP_TYP из \_S5 DSDT в PM1a/PM1b_CNT с SLP_EN) и перезагрузка (регистр сброса FADT,
   затем порт 0xCF9, затем контроллер клавиатуры 8042, последняя мера - тройная ошибка). */
#include "../../platform.h"
#include "../../acpi.h"
#include "../../uart.h"
#include "../../vmm.h"
#include "cpu.h"

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v) { __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }

void platform_poweroff(void) {
    const acpi_info_t* a = acpi_get();
    if (!a || !a->s5_valid || !a->pm1a_cnt || a->pm1a_cnt > 0xFFFF) { uart_print("[KIRA ACPI] no \\_S5 or PM1 control port: cannot power off.\n"); return; }
    uint16_t pm1a = (uint16_t)a->pm1a_cnt;
    if (!(inw(pm1a) & 1) && a->smi_cmd && a->acpi_enable) {      /* SCI_EN = 0: прошивка держит SMM - в режим ACPI */
        outb((uint16_t)a->smi_cmd, a->acpi_enable);
        for (int i = 0; i < 3000000 && !(inw(pm1a) & 1); i++) cpu_relax();
    }
    uart_print("[KIRA ACPI] powering off (S5)\n");
    uart_flush_all();
    cpu_irq_off();
    outw(pm1a, (uint16_t)((a->slp_typa & 7) << 10 | 1u << 13));
    if (a->pm1b_cnt && a->pm1b_cnt <= 0xFFFF) outw((uint16_t)a->pm1b_cnt, (uint16_t)((a->slp_typb & 7) << 10 | 1u << 13));
    for (int i = 0; i < 100000000; i++) cpu_relax();
    cpu_irq_on();
    uart_print("[KIRA ACPI] machine did not power off after S5.\n");
}

void platform_reboot(void) {
    const acpi_info_t* a = acpi_get();
    uart_print("[KIRA ACPI] rebooting\n");
    uart_flush_all();
    cpu_irq_off();
    if (a && a->reset_valid) {                                    /* регистр сброса FADT */
        if (a->reset_space == 1 && a->reset_addr <= 0xFFFF) outb((uint16_t)a->reset_addr, a->reset_value);
        else if (a->reset_space == 0) { volatile uint8_t* r = (volatile uint8_t*)vmm_map_mmio(a->reset_addr, 1, VMM_MMIO_UC); if (r) *r = a->reset_value; }
        for (int i = 0; i < 10000000; i++) cpu_relax();
    }
    outb(0xCF9, 0x02); for (int i = 0; i < 1000; i++) cpu_io_delay(); outb(0xCF9, 0x06);   /* сброс через PCI */
    for (int i = 0; i < 10000000; i++) cpu_relax();
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) cpu_relax();                    /* 8042: импульс сброса */
    outb(0x64, 0xFE);
    for (int i = 0; i < 10000000; i++) cpu_relax();
    struct { uint16_t lim; uint64_t base; } __attribute__((packed)) z = { 0, 0 };        /* тройная ошибка */
    __asm__ volatile ("lidt %0\n int3" : : "m"(z));
}

/* Экран x86-64 - от загрузчика (multiboot) или virtio-gpu: своего платформенного нет. */
int platform_fb_probe(void) { return -1; }
