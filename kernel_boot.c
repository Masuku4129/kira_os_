#include "kernel.h"
#include "cpu.h"
#include "uart.h"
#include "gdt.h"
#include "idt.h"
#include "pic.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "apic.h"
#include "immune.h"
#include "mind_sys.h"
#include "watchdog.h"
#include "keyboard.h"
#include "console.h"
#include "sched.h"
#include "timer.h"
#include "fpu.h"
#include "clock.h"
#include "display.h"
#include "virtio_input.h"
#include "e1000.h"
#include "virtio_net.h"
#include "net.h"
#include "hda.h"
#include "acpi.h"
#include "hwprofile.h"
#include "ramext.h"
#include "percpu.h"
#include "smp.h"
#include "proc.h"
#include "syscall.h"
#include "life.h"
#include "fsrv.h"
#include "experience.h"
#include "irqmode.h"
#include "mbinfo.h"
#include "earlycon.h"
#include "rtc.h"
#include "usb.h"
#include "rng.h"
#include "ac97.h"
#include "sb16.h"
#include "uaccess.h"
#include "fat.h"

/* FAT timestamps use local RTC time, matching removable-media conventions. */
static void fat_clock(uint16_t* date, uint16_t* time) {
    rtc_time_t t;
    if (rtc_now(mind_sys_local_offset(), &t) != 0 || t.year < 1980 || t.year > 2107) {
        *date = (uint16_t)((2026 - 1980) << 9 | 1 << 5 | 1);
        *time = 0;
        return;
    }
    *date = (uint16_t)((t.year - 1980) << 9 | t.month << 5 | t.day);
    *time = (uint16_t)(t.hour << 11 | t.min << 5 | t.sec / 2);
}

/* ACPI may live in reserved physical memory outside the normal identity map. */
static const void* acpi_map(uint64_t phys, uint64_t len) {
    if (ramext_map_read(phys, len)) return (const void*)(uintptr_t)phys;
    return (const void*)vmm_map_mmio(phys, len, VMM_MMIO_UC);
}

/*
 * x86-64 boot composition root.
 *
 * Keep the ordering here explicit until each subsystem has a versioned init
 * contract and failure-unwind semantics. This function intentionally preserves
 * the established boot sequence; it is not yet a service manager.
 */
void kernel_boot_init(unsigned long magic, unsigned long addr) {
    percpu_install(&percpu_table[0]);
    uart_init();
    uart_print("\n==================================================\n");
    uart_print("[KIRA OS] Kernel core starting...\n");
    if (!uart_present()) uart_print("[KIRA UART] No COM1 serial port: the log goes to the screen only, commands - from the keyboard.\n");
    uart_print("==================================================\n");

    /* UEFI may place the Multiboot2 structure above 1 GiB; relocate it first. */
    addr = mbinfo_relocate(magic, addr);
    if (earlycon_init(addr) == 0) uart_print("[KIRA BOOT] Early console is on screen.\n");

    gdt_init();
    percpu_init_bsp(0);
    pic_init();
    idt_init();

    if (fpu_init() != 0) kernel_panic("FPU/SSE initialization failed");
    if (pmm_init(magic, addr) != 0) kernel_panic("PMM initialization failed");
    if (vmm_init() != 0) kernel_panic("VMM initialization failed");

    if (vmm_guard_null_page() != 0) kernel_panic("Cannot install NULL guard page");
    {
        uint64_t pa;
        if (vmm_translate(0, &pa) == 0) kernel_panic("NULL guard page check failed");
        uart_print("[KIRA VMM] NULL guard installed: first 2 MB split into 4 KB pages, page 0 unmapped.\n");
    }

    {
        int smep = vmm_enable_smep();
        uart_print(smep ? "[KIRA VMM] SMEP enabled: kernel cannot execute user pages.\n"
                        : "[KIRA VMM] SMEP not offered by this CPU.\n");
        int smap = uaccess_enable_smap();
        uart_print(smap ? "[KIRA VMM] SMAP enabled: kernel touches user memory only through checked copies.\n"
                        : "[KIRA VMM] SMAP not offered by this CPU (user copies are still checked and fault-safe).\n");
    }

    ramext_init();
    acpi_set_map_hook(acpi_map);
    acpi_init(addr);
    {
        const acpi_info_t* ai = acpi_get();
        hw_init(ai && ai->valid ? ai->cpu_count : 1, pmm_get_total_memory_kb());
        hw_describe(hw_get(), uart_print);
    }

    heap_init();
    sched_init(hw_get()->cpus, hw_get()->ram_kb);
    immune_init();
    watchdog_init();
    console_init();
    proc_init(hw_get()->cpus, hw_get()->ram_kb);
    syscall_init();
    experience_init();
    fs_init();
    life_init();
    mind_sys_init();

    /* Keep a usable text/serial console if graphics initialization fails. */
    earlycon_stop();
    if (display_init(addr) != 0) {
        int ec_rc = earlycon_init(addr);
        uart_print("[KIRA DISPLAY] Running without the graphical shell (");
        uart_print(ec_rc == 0 ? "text console on screen" : "serial console only");
        uart_print(").\n");
    }

    if (keyboard_init() != 0) kernel_panic("Keyboard IRQ registration failed");
    mouse_init();
    fat_set_clock(fat_clock);
    rng_init();
    usb_init();
    virtio_input_init();
    if (e1000_probe() != 0) virtio_net_probe();
    net_start();
    if (hda_probe() != 0 && ac97_probe() != 0) sb16_probe();
    if (timer_init(TIMER_HZ) != 0) kernel_panic("Timer initialization failed");

    /* Calibrate the timebase before enabling interrupts and scheduler ticks. */
    const char* tsc_method = "";
    uint64_t tsc_khz_val = clock_calibrate(&tsc_method);
    const acpi_info_t* ai = acpi_get();
    int have_apic = (apic_init(mbinfo_cmdline_has(addr, "x2apic"), ai && ai->valid ? ai->max_apic_id : 0) == 0);
    if (have_apic) percpu_table[0].apic_id = apic_get_id();
    else uart_print("[KIRA APIC] Continuing without Local APIC (PIT only).\n");

    uart_print("[KIRA OS] Core initialization complete. Enabling interrupts.\n");
    __asm__ volatile ("sti");

    {
        uint64_t t = timer_get_ticks();
        uint64_t t_wait = rdtsc();
        while (timer_get_ticks() == t) {
            if (rdtsc() - t_wait > 4000000000ULL) {
                uart_print("[KIRA TIMER] No timer interrupt arrived (IRQ0 via PIT/PIC, IOAPIC not used yet).\n");
                uart_print("[KIRA TIMER] The scheduler cannot run without a tick; check the firmware's legacy PIT/8259 support.\n");
                kernel_panic("No timer tick after enabling interrupts");
            }
            __asm__ volatile ("pause");
        }
        if (!tsc_khz_val) {
            uint64_t start_tick = timer_get_ticks();
            uint64_t c0 = rdtsc();
            while (timer_get_ticks() < start_tick + TIMER_HZ / 10) __asm__ volatile ("hlt");
            tsc_khz_val = (rdtsc() - c0) * TIMER_HZ / (TIMER_HZ / 10) / 1000;
            tsc_method = "timer ticks over 100 ms (coarse, up to 2%)";
        }
        sched_set_tsc_khz(tsc_khz_val);
        hw_set_tsc_khz((uint32_t)tsc_khz_val);
        clock_init(tsc_khz_val);
        if (rtc_init() != 0) uart_print("[KIRA RTC] No valid real-time clock: the top bar shows no date.\n");
        uart_print("[KIRA CPU] TSC ");
        uart_print_dec(tsc_khz_val);
        uart_print(" kHz, measured by ");
        uart_print(tsc_method);
        uart_print(".\n");
    }

    uint64_t apic_rate = 0;
    if (have_apic) {
        apic_rate = apic_calibrate_timer_tsc(tsc_khz_val);
        console_set_apic_rate(apic_rate);
        if (apic_rate) {
            uart_print("[KIRA APIC] Timer calibrated against TSC: ");
            uart_print_dec(apic_rate / 1000);
            uart_print(" kcounts/s (bus clock ~");
            uart_print_dec(apic_rate * 16 / 1000000);
            uart_print(" MHz).\n");
        } else {
            uart_print("[KIRA APIC] Timer calibration failed.\n");
        }
    }

    irqmode_enable_apic(tsc_khz_val, apic_rate, have_apic, TIMER_HZ);
    smp_start(acpi_get());
    uart_print("[KIRA OS] Ready. Type 'help' and press Enter.\n");
}
