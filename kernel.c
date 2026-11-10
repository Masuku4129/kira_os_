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
#include "bootmap.h"
#include "percpu.h"
#include "smp.h"
#include "proc.h"
#include "syscall.h"
#include "fscmd.h"
#include "life.h"
#include "progs.h"
#include "kdrv.h"
#include "fsrv.h"
#include "experience.h"
#include "irqmode.h"
#include "mbinfo.h"
#include "bootinfo.h"
#include "earlycon.h"
#include "spinlock.h"
#include "rtc.h"
#include "usb.h"
#include "rng.h"
#include "ac97.h"
#include "sb16.h"
#include "uaccess.h"
#include "fat.h"
#include "iommu.h"
#include "pci.h"

#define SELF_CHECK_PERIOD_S 5

void kernel_panic(const char* reason) {
    __asm__ volatile ("cli");
    smp_halt_others();  // остальные ядра встают (второе паникующее ядро встанет молча)
    spin_bust_locks();  // замок мог держать упавший код: аварийный вывод не должен на нём зависнуть
    earlycon_alert();   // на раннем экране паника красным
    uart_print("\n[KIRA KERNEL PANIC] ");
    uart_print(reason);
    uart_print("\n[KIRA KERNEL PANIC] called from ");
    uart_print_hex((uint64_t)(uintptr_t)__builtin_return_address(0));
    uart_print("\n");
    uart_flush_all();        // накопленное в построчных буферах всех ядер
    display_panic_flush();   // то же самое - на экран (если он есть)
    for (;;) {
        __asm__ volatile ("hlt");
    }
}


/* Метки времени файлов на флешках: местное время по RTC (FAT хранит местное время, как Windows и Android). */
static void fat_clock(uint16_t* date, uint16_t* time) {
    rtc_time_t t;
    if (rtc_now(mind_sys_local_offset(), &t) != 0 || t.year < 1980 || t.year > 2107) { *date = (uint16_t)((2026 - 1980) << 9 | 1 << 5 | 1); *time = 0; return; }
    *date = (uint16_t)((t.year - 1980) << 9 | t.month << 5 | t.day);
    *time = (uint16_t)(t.hour << 11 | t.min << 5 | t.sec / 2);
}

/* Таблицы ACPI выше тождественной карты: в памяти типа «ACPI» - отображаем тождественно (только чтение), иначе
   (SeaBIOS и многие прошивки кладут их в «зарезервированную» память у верхней границы ОЗУ) - через окно MMIO ядра
   без кэширования: так читается что угодно где угодно, не трогая чужие области обычной картой. */
static const void* acpi_map(uint64_t phys, uint64_t len) {
    if (ramext_map_read(phys, len)) return (const void*)(uintptr_t)phys;
    return (const void*)vmm_map_mmio(phys, len, VMM_MMIO_UC);
}

extern char kernel_start[];
extern char kernel_end[];

static uint8_t uefi_compat_mbi[65536] __attribute__((aligned(8)));
static uint64_t uefi_rsdp_override;

void kernel_uefi_entry(const void *blob, uint32_t blob_size) {
    uint32_t mbi_size = 0;
    const kbi_header_t *h = (const kbi_header_t *)blob;

    percpu_install(&percpu_table[0]);
    uart_init();
    if (kbi_validate(blob, blob_size) != 0 ||
        h->arch != KBI_ARCH_X86_64 || h->boot_mode != KBI_BOOT_UEFI) {
        uart_print("[KIRA UEFI] Invalid loader-neutral BootInfo; refusing to start.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    uefi_rsdp_override = h->rsdp_phys;
    uart_print("[KIRA UEFI] Boot modules: ");
    uart_print_dec(h->modules_count);
    uart_print(", command line: ");
    uart_print((h->flags & KBI_F_CMDLINE) ? "present\n" : "absent\n");
    {
        const kbi_module_t *modules = kbi_modules(blob);
        for (uint32_t i = 0; i < h->modules_count; ++i) {
            uart_print("[KIRA UEFI] Module ");
            uart_print_dec(i);
            uart_print(": ");
            uart_print((const char *)blob + modules[i].name_off);
            uart_print(" bytes=");
            uart_print_dec(modules[i].length);
            uart_print(" magic=");
            if (modules[i].base < (1ull << 30) &&
                modules[i].length >= sizeof(uint32_t) &&
                modules[i].length <= (1ull << 30) - modules[i].base)
                uart_print_hex(*(const uint32_t *)(uintptr_t)modules[i].base);
            else
                uart_print("not-identity-mapped");
            uart_print("\n");
        }
    }
    if (kbi_to_multiboot2(blob, blob_size, uefi_compat_mbi,
                          sizeof(uefi_compat_mbi), &mbi_size) != 0) {
        uart_print("[KIRA UEFI] Cannot translate BootInfo into the kernel's current boot-tag view.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }
    uart_print("[KIRA UEFI] Common BootInfo accepted; entering shared kernel initialization.\n");
    kernel_main(0x36D76289UL, (unsigned long)(uintptr_t)uefi_compat_mbi);
}

void kernel_main(unsigned long magic, unsigned long addr) {
    percpu_install(&percpu_table[0]);   /* учёт замков и вывод обращаются к данным ядра с первой строки */
    uart_init();
    uart_print("\n==================================================\n");
    uart_print("[KIRA OS] Kernel core starting...\n");
    if (!uart_present()) uart_print("[KIRA UART] No COM1 serial port: the log goes to the screen only, commands - from the keyboard.\n");
    uart_print("==================================================\n");

    // Загрузчик (UEFI) мог положить Multiboot2-структуру выше 1 ГБ: переносим её в ядро
    // и сразу включаем раннюю консоль на экране, чтобы любой сбой дальше был виден.
    addr = mbinfo_relocate(magic, addr);
    const void* bootinfo = 0;
    uint32_t bootinfo_size = 0;
    uint32_t mb2_size = 0;
    if (magic == 0x36D76289UL && addr && addr < (1UL << 30) && (addr & 7) == 0) {
        mb2_size = *(const uint32_t*)(uintptr_t)addr;
        int bi_rc = kbi_from_multiboot2((const void*)(uintptr_t)addr, mb2_size,
            (uint64_t)(uintptr_t)kernel_start,
            ((uint64_t)(uintptr_t)kernel_end - (uint64_t)(uintptr_t)kernel_start),
            &bootinfo, &bootinfo_size);
        if (bi_rc != 0) {
            uart_print("[KIRA BOOT] Multiboot2 to BootInfo conversion rejected: ");
            uart_print_dec((uint32_t)(-bi_rc)); uart_print(".\n");
            kernel_panic("BootInfo validation failed");
        }
        const kbi_header_t* bi = (const kbi_header_t*)bootinfo;
        uart_print("[KIRA BOOT] BootInfo v"); uart_print_dec(bi->version);
        uart_print(" validated: memory ranges="); uart_print_dec(bi->mmap_count);
        uart_print(", modules="); uart_print_dec(bi->modules_count);
        uart_print(", bytes="); uart_print_dec(bootinfo_size); uart_print(".\n");
    }
    if (earlycon_init(addr) == 0) uart_print("[KIRA BOOT] Early console is on screen.\n");

    // Каждый модуль печатает "online" только после реальной проверки;
    // при отказе ядро останавливается с диагностикой, а не идёт дальше.
    gdt_init();
    percpu_init_bsp(0);              /* GS = данные процессора; после gdt_init, она перезагружает GS */
    pic_init();
    idt_init();

    if (fpu_init() != 0)            kernel_panic("FPU/SSE initialization failed");

    if (bootinfo) {
        if (pmm_init_bootinfo(bootinfo, bootinfo_size, addr, mb2_size) != 0)
            kernel_panic("PMM initialization from BootInfo failed");
    } else if (pmm_init(magic, addr) != 0) kernel_panic("PMM initialization failed");
    if (vmm_init() != 0)            kernel_panic("VMM initialization failed");

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
    bootmap_harden();               /* окно раннего отображения строилось до NXE: без NX */
    if (vmm_enforce_wx() != 0)      kernel_panic("W^X for kernel memory could not be enforced");
    acpi_set_map_hook(acpi_map);
    if (uefi_rsdp_override) acpi_init_rsdp(uefi_rsdp_override);
    else acpi_init(addr);
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
    experience_init();   // опыт прошлых жизней: если есть диск, организм вспоминает
    fs_init();           // файловая система на том же диске (после хранилища опыта)
    life_init();         // журнал организма и профили программ в /var
    mind_sys_init();     // Разум: нормы, профили и выученный квант из /var/mind*

    // Экран необязателен. Раннее окно снимается до того, как драйвер дисплея сам
    // отобразит видеопамять; если драйвер не смог (формат пикселей, память), ранняя
    // консоль возвращается и показывает весь журнал, а не оставляет чёрный экран.
    earlycon_stop();
    /* PCI DMA policy must be active before display_init can probe a virtio GPU. */
    int pci_devices = pci_init();
    uart_print("[KIRA PCI] "); uart_print_dec((uint64_t)pci_devices); uart_print(" device(s) found.\n");
    (void)iommu_init();
    if (display_init(addr) != 0) {
        int ec_rc = earlycon_init(addr);
        uart_print("[KIRA DISPLAY] Running without the graphical shell (");
        uart_print(ec_rc == 0 ? "text console on screen" : "serial console only");
        uart_print(").\n");
    }

    if (keyboard_init() != 0)       kernel_panic("Keyboard IRQ registration failed");
    mouse_init();   // необязательна: без мыши клавиатура работает как раньше
    fat_set_clock(fat_clock);
    rng_init();            // генератор случайных чисел: RDSEED/RDRAND, virtio-rng, времена прерываний
    usb_init();
    virtio_input_init();
    if (e1000_probe() != 0) virtio_net_probe();   /* сетевая карта: Intel e1000 или virtio-net */         // сетевая карта Intel e1000 (Limbo, QEMU, многие ПК)
    net_start();
    if (hda_probe() != 0 && ac97_probe() != 0) sb16_probe();   // звук: Intel HDA, иначе AC'97 (Limbo: Sound ac97), иначе Sound Blaster 16 (ISA)           // стек: ARP, IPv4, ICMP, UDP, DHCP - свой поток   // клавиатура/мышь/планшет virtio (если машина их даёт)     // USB: клавиатуры, мыши, планшеты, хабы, флешки (UHCI, xHCI): свой поток, работает рядом с PS/2
    if (timer_init(TIMER_HZ) != 0)  kernel_panic("Timer initialization failed");

    // Частота TSC - до прерываний и без тика: канал 2 PIT / CPUID 0x15 (см. clock.c). От неё зависят часы,
    // кванты по времени и период таймера LAPIC, поэтому меряем точно (раньше - 100 мс по тикам 2 мс, ошибка до 2%).
    const char* tsc_method = "";
    uint64_t tsc_khz_val = clock_calibrate(&tsc_method);

    // APIC необязателен: при отказе работаем на PIT. x2APIC - если его включила прошивка, если в MADT есть
    // APIC ID больше 254 или по параметру загрузки x2apic.
    int have_apic;
    {
        const acpi_info_t* ai = acpi_get();
        have_apic = (apic_init(mbinfo_cmdline_has(addr, "x2apic"), ai && ai->valid ? ai->max_apic_id : 0) == 0);
    }
    if (have_apic) percpu_table[0].apic_id = apic_get_id();   /* настоящий ID загрузочного ядра (не обязательно 0) */
    else uart_print("[KIRA APIC] Continuing without Local APIC (PIT only).\n");

    uart_print("[KIRA OS] Core initialization complete. Enabling interrupts.\n");
    __asm__ volatile ("sti");

    {
        uint64_t t = timer_get_ticks();
        // Ждём первый тик без hlt: если IRQ0 не доставляется (маршрутизация PIC/IOAPIC,
        // особенности платы), hlt спал бы вечно и экран остался бы без объяснения.
        uint64_t t_wait = rdtsc();
        while (timer_get_ticks() == t) {
            if (rdtsc() - t_wait > 4000000000ULL) {   // не меньше ~1 с на любом x86-64
                uart_print("[KIRA TIMER] No timer interrupt arrived (IRQ0 via PIT/PIC, IOAPIC not used yet).\n");
                uart_print("[KIRA TIMER] The scheduler cannot run without a tick; check the firmware's legacy PIT/8259 support.\n");
                kernel_panic("No timer tick after enabling interrupts");
            }
            __asm__ volatile ("pause");
        }
        if (!tsc_khz_val) {                              // последний запасной путь: 100 мс по тикам (грубо)
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

    // Современная схема прерываний: IOAPIC + таймер LAPIC. Все шаги проверяются, при сбое - остаёмся на PIC/PIT.
    irqmode_enable_apic(tsc_khz_val, apic_rate, have_apic, TIMER_HZ);

    {
        const acpi_info_t* ai = acpi_get();
        smp_start(ai);                       // остальные ядра: каждое входит в планировщик со своим тиком LAPIC
        if (vmm_verify_wx(0) != 0) kernel_panic("W^X violated after start-up of the other cores");
    }

    uart_print("[KIRA OS] Ready. Type 'help' and press Enter.\n");

    // Поток "core": опрос консоли, секундные метрики, самоконтроль.
    // Нагрузка CPU берётся из учёта планировщика (такты TSC в idle-потоке).
    uint64_t handled = 0;
    uint64_t seconds = 0;
    uint64_t last_verify = 0;

    for (;;) {
        console_poll();
        prog_poll();         // запуск, заказанный дисплеем (иконки дока)
        kdrv_poll();         // драйверы Ring 3: первое сканирование /drv, возврат памяти погибших
        (void)iommu_fault_poll();   // нарушения DMA, записанные VT-d: строка в журнал, без остановки ядра
        life_tick();         // журнал организма: запись накопленных событий

        uint64_t now = timer_get_ticks();
        unsigned pending = 0;
        while (handled < now) {
            handled++;
            if (handled % TIMER_HZ == 0) pending++;
        }

        if (pending) {
            /* Раз в секунду - шаг Разума: замеры всей системы, нормы, решения (если секунд накопилось несколько -
               основной поток был занят, - шаг один: замеры всё равно за весь промежуток). */
            seconds += pending;
            (void)console_take_events();
            mind_sys_tick(seconds);
            experience_tick(seconds);
            sched_reap();

            // Рефлекс самоконтроля: целостность кода ядра
            if (seconds - last_verify >= SELF_CHECK_PERIOD_S) {
                last_verify = seconds;
                if (immune_verify_self() != 0) {
                    kernel_panic("IMMUNE: kernel code/rodata digest mismatch (self-integrity violated)");
                }
            }
        }

        watchdog_feed();
        {   /* ядро было прервано с GS процесса: такого окна быть не должно - сообщить, где (раз на новое значение) */
            extern volatile uint64_t gs_fixups, gs_fixup_rip; extern volatile uint32_t gs_fixup_vec;
            static uint64_t seen_fix;
            uint64_t n = gs_fixups;
            if (n != seen_fix) {
                seen_fix = n;
                uart_print("[KIRA GS] kernel entered with the process GS: vector "); uart_print_dec(gs_fixup_vec);
                uart_print(" at RIP "); uart_print_hex(gs_fixup_rip); uart_print(", times "); uart_print_dec(n); uart_print("\n");
            }
        }
        {   /* машина стояла целиком (хозяин приостановил ВМ): сообщить, сторож этот промежуток не засчитал */
            extern volatile uint64_t watchdog_stalls, watchdog_stall_max;
            static uint64_t seen_stalls;
            uint64_t n = watchdog_stalls;
            if (n != seen_stalls) {
                seen_stalls = n;
                uart_print("[KIRA WATCHDOG] the whole machine stood still (longest "); uart_print_dec(watchdog_stall_max * 1000 / TIMER_HZ);
                uart_print(" ms, times "); uart_print_dec(n);
                uart_print("): not counted as a hang. Either the host paused the VM or this code ran with interrupts off: core ");
                extern uint64_t watchdog_stall_where[]; extern uint32_t watchdog_stall_cpu;
                uart_print_dec(watchdog_stall_cpu); uart_print(" at");
                for (int k = 0; k < 6 && watchdog_stall_where[k]; k++) { uart_print(" "); uart_print_hex(watchdog_stall_where[k]); }
                uart_print("\n");
            }
        }
        sched_sleep_ticks(TIMER_PER_10MS);
    }
}
