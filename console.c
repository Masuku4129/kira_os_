#include "console.h"
#include "net.h"
#include "hda.h"
#include "input.h"
#include "proc.h"
#include "progs.h"
#include "life.h"
#include "gfxsrv.h"
#include "experience.h"
#include "platform.h"
#include "smp.h"
#include "uart.h"
#include "timer.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "immune.h"
#include "mind_sys.h"
#include "watchdog.h"
#include "sched.h"
#include "clock.h"
#include "display.h"
#include "fscmd.h"
#include "pci.h"
#include "blk.h"
#include "vfs.h"
#include "rng.h"
#include "spinlock.h"
#include "usb.h"

static volatile uint32_t event_count;

void console_note_event(void) {
    event_count++;
}

uint32_t console_take_events(void) {
    uint32_t n = event_count;
    event_count = 0;
    return n;
}

void console_init(void) {
    uart_print("[KIRA CONSOLE] Command line ready: type 'help' and press Enter.\n");
}

static void print_status(void) {
    heap_stats_t hs;
    heap_get_stats(&hs);

    uart_print("\n[STATUS] uptime ticks: ");
    uart_print_dec(timer_get_ticks());
    if (clock_ready()) { uart_print(", precise clock ms: "); uart_print_dec(clock_ms()); }
    uart_print("\n[STATUS] PMM free: ");
    uart_print_dec(pmm_get_free_memory_kb() >> 10);
    uart_print(" MB of ");
    uart_print_dec(pmm_get_total_memory_kb() >> 10);
    uart_print(" MB\n[STATUS] kernel heap: ");
    uart_print_dec(hs.used_blocks);
    uart_print(" blocks, ");
    uart_print_dec(hs.used_bytes >> 10);
    uart_print(" KB in use; pages: ");
    uart_print_dec(hs.slab_pages);
    uart_print(" small-block, ");
    uart_print_dec(hs.large_pages);
    uart_print(" large-block");
    if (hs.bad_frees) { uart_print("; rejected frees: "); uart_print_dec(hs.bad_frees); }
    {
        mind_view_t* v = (mind_view_t*)kmalloc(sizeof(mind_view_t));
        if (v) {
            mind_sys_view(v);
            uart_print("\n[STATUS] mind: health ");
            uart_print_dec(v->health); uart_print(" ("); uart_print(v->mood); uart_print("), mode ");
            uart_print(v->mode ? "act" : "observe");
            uart_print(", quantum "); uart_print_dec(v->quantum_ms); uart_print(" ms, decisions ");
            uart_print_dec(v->decisions);
            kfree(v);
        }
    }
    uart_print("\n[STATUS] immune: kernel integrity ");
    uart_print(immune_verify_self() == 0 ? "OK" : "VIOLATED");
    uart_print(", antibodies ");
    uart_print_dec(immune_antibody_count());
    uart_print(", quarantine ");
    uart_print_dec(immune_quarantine_count());
    uart_print("\n[STATUS] ");
    platform_status(uart_print);                          /* контроллер прерываний и таймер этой архитектуры */
    uart_print("\n");
}

typedef struct {
    uint32_t duration_ticks;
} burner_arg_t;

static burner_arg_t burner_args[8];
static unsigned burner_slot;
static uint32_t burner_serial;

// Рабочий поток: крутит счётчик заданное время. Счётчик итераций показывает,
// сколько процессорного времени получил поток.
static void burner_main(void* arg) {
    const burner_arg_t* a = (const burner_arg_t*)arg;
    uint64_t end = timer_get_ticks() + a->duration_ticks;
    uint64_t n = 0;
    while (timer_get_ticks() < end) {
        n++;
    }
    uart_print("[");
    uart_print(sched_current()->name);
    uart_print("] finished, iterations: ");
    uart_print_dec(n);
    uart_print("\n");
}

static void spawn_burners(unsigned count, uint32_t seconds, thread_prio_t prio) {
    for (unsigned i = 0; i < count; i++) {
        burner_arg_t* a = &burner_args[burner_slot++ % 8];
        a->duration_ticks = seconds * TIMER_HZ;
        char name[SCHED_NAME_LEN] = "burn-";
        uint32_t id = ++burner_serial;
        unsigned pos = 5;
        char digits[12]; unsigned nd = 0;
        do { digits[nd++] = (char)('0' + id % 10); id /= 10; } while (id && nd < 10);
        while (nd && pos < SCHED_NAME_LEN - 1) name[pos++] = digits[--nd];
        name[pos] = '\0';
        if (!sched_create(name, burner_main, a, prio)) {
            uart_print("[SCHED] cannot create thread (no free slot/memory)\n");
            return;
        }
    }
}

static void self_test_memory(void) {
    uart_print("\n[TEST] heap/PMM self-test...\n");
    heap_stats_t before, after;
    heap_get_stats(&before);

    enum { N = 64 };
    uint8_t* blk[N];
    unsigned sizes[N];
    int ok = 1;

    for (int i = 0; i < N; i++) {
        sizes[i] = (unsigned)((i * 37) % 500) + 1;
        blk[i] = (uint8_t*)kmalloc(sizes[i]);
        if (!blk[i]) { ok = 0; break; }
        for (unsigned k = 0; k < sizes[i]; k++) blk[i][k] = (uint8_t)(i + k);
    }
    // освобождаем сначала чётные, потом нечётные - проверка слияния блоков
    for (int pass = 0; pass < 2; pass++) {
        for (int i = pass; i < N; i += 2) {
            if (!blk[i]) continue;
            for (unsigned k = 0; k < sizes[i]; k++) {
                if (blk[i][k] != (uint8_t)(i + k)) ok = 0; // данные не должны портиться соседями
            }
            kfree(blk[i]);
        }
    }
    heap_get_stats(&after);
    int heap_ok = ok && after.used_blocks == before.used_blocks && after.used_bytes == before.used_bytes &&
                  after.bad_frees == before.bad_frees;
    uart_print(heap_ok ? "[TEST] heap: PASS (data intact, every block returned)\n"
                       : "[TEST] heap: FAIL\n");

    // Счёт свободных страниц меняют и другие ядра (куча, программы), поэтому точный учёт здесь не сравнивается
    // (его проверяет хост-тест PMM). Проверяется: страницы разные и выровнены, в них можно писать, освобождение
    // проходит, освобождение страницы образа ядра отвергается. (Повторное освобождение здесь не пробуем: страницу
    // за это время могло получить другое ядро - его отвергание проверяет хост-тест.)
    uint64_t pages[16];
    int pmm_ok = 1;
    for (int i = 0; i < 16; i++) {
        pages[i] = pmm_alloc_page();
        if (!pages[i] || (pages[i] & 0xFFF)) { pmm_ok = 0; continue; }
        for (int k = 0; k < i; k++) if (pages[k] == pages[i]) pmm_ok = 0;
        *(volatile uint64_t*)(uintptr_t)pages[i] = 0x4B495241ULL + (uint64_t)i;
    }
    for (int i = 0; i < 16; i++) {
        if (!pages[i]) continue;
        if (*(volatile uint64_t*)(uintptr_t)pages[i] != 0x4B495241ULL + (uint64_t)i) pmm_ok = 0;
        if (pmm_free_page(pages[i]) != 0) pmm_ok = 0;
    }
    extern char kernel_start[];
    pmm_ok = pmm_ok && (pmm_free_page((uint64_t)(uintptr_t)kernel_start & ~0xFFFULL) == -1);   // страница образа ядра
    uart_print(pmm_ok ? "[TEST] PMM: PASS (distinct writable pages, freed, kernel-image free refused)\n"
                      : "[TEST] PMM: FAIL\n");

    int guard_ok = 1;
    uint64_t pa;
    guard_ok = guard_ok && (vmm_translate(0, &pa) != 0);              // NULL не отображён
    guard_ok = guard_ok && (vmm_translate(0x1000, &pa) == 0 && pa == 0x1000);
    uart_print(guard_ok ? "[TEST] VMM: PASS (NULL page unmapped, page 1 identity)\n\n"
                        : "[TEST] VMM: FAIL\n\n");
}

static const char* verdict_name(immune_verdict_t v) {
    return v == IMMUNE_SELF ? "SELF" : (v == IMMUNE_HOSTILE ? "HOSTILE" : "UNKNOWN");
}

static void immune_demo(void) {
    uart_print("\n[IMMUNE] demo\n");

    // 1. Собственный код ядра: сверка с эталоном, снятым при загрузке
    uart_print("[IMMUNE] kernel self-integrity: ");
    uart_print(immune_verify_self() == 0 ? "intact\n" : "VIOLATED\n");

    // 2. Шелл-код с NOP-sled
    uint8_t sled[64];
    for (int i = 0; i < 64; i++) sled[i] = 0x90;
    sled[60] = 0xCC;
    immune_verdict_t v1 = immune_classify(sled, sizeof(sled));
    uart_print("[IMMUNE] NOP-sled buffer -> ");
    uart_print(verdict_name(v1));
    uart_print("\n");

    // 3. Строка /bin/sh внутри "драйвера"
    uint8_t blob[32] = { 0x55, 0x48, 0x89, 0xE5, '/', 'b', 'i', 'n', '/', 's', 'h', 0, 0xC3 };
    immune_verdict_t v2 = immune_classify(blob, sizeof(blob));
    uart_print("[IMMUNE] buffer with /bin/sh -> ");
    uart_print(verdict_name(v2));
    uart_print("\n");

    // 4. Безобидный, но чужой код: допуска нет
    uint8_t clean[16] = { 0x55, 0x48, 0x89, 0xE5, 0x31, 0xC0, 0x5D, 0xC3 };
    immune_verdict_t v3 = immune_classify(clean, sizeof(clean));
    uart_print("[IMMUNE] unknown clean code -> ");
    uart_print(verdict_name(v3));
    uart_print("\n");

    // 5. Если зарегистрировать дайджест как "свой", тот же код становится своим
    uint8_t digest[32];
    sha256(clean, sizeof(clean), digest);
    immune_register_self(digest);
    immune_verdict_t v4 = immune_classify(clean, sizeof(clean));
    uart_print("[IMMUNE] same code after registering its digest -> ");
    uart_print(verdict_name(v4));
    uart_print("\n");

    // 6. Фагоцитоз враждебного буфера
    immune_phagocytose(sled, sizeof(sled));
    int wiped = 1;
    for (int i = 0; i < 64; i++) if (sled[i] != 0) wiped = 0;
    uart_print(wiped ? "[IMMUNE] hostile buffer wiped (phagocytosis)\n" : "[IMMUNE] wipe FAILED\n");

    uart_print("[IMMUNE] quarantine entries: ");
    uart_print_dec(immune_quarantine_count());
    uart_print("\n\n");
}

/* ---- Командная строка ------------------------------------------------------------------
   Символы накапливаются в строке, команда выполняется по Enter. Работают: Backspace, Ctrl+U (стереть
   строку), Ctrl+C (отмена), Ctrl+L (очистить экран), стрелки вверх/вниз (история). Источники ввода те же:
   UART и клавиатура (экран QEMU/VNC). */

#define LINE_MAX 512
#define HIST_N 8
#define PROMPT "кира> "

static char line[LINE_MAX];
static unsigned line_len;
static char hist[HIST_N][LINE_MAX];
static unsigned hist_n, hist_pos;
static int prompt_shown;

typedef struct {
    const char* name;
    char alias;                    /* однобуквенный вариант (0 - нет); опасные тесты вызываются только полным именем */
    const char* help;
    void (*fn)(const char* arg);
} cmd_t;

static void cmd_help(const char* arg);

static void prog_start(const char* name) {
    int r = prog_run(name);
    if (r == -3) uart_print("[PROG] файл не принят как программа (см. сообщение выше)\n");
}

static void cmd_clear(const char* a) {
    (void)a;
    uart_putchar('\f');                                 /* экран: управляющий код в журнале, порядок вывода сохраняется */
    uart_print_serial("\x1b[2J\x1b[H");                  /* терминал на другом конце UART */
}
static void cmd_status(const char* a) { (void)a; print_status(); }
static void cmd_ps(const char* a) { (void)a; sched_dump(); }
static void prog_start(const char* name);
static void cmd_burn(const char* a) { (void)a; uart_print("[LOAD] 1 burn process for 3 s; 'core' keeps running (preemptive scheduling)\n"); prog_start("burn"); }
static void cmd_burnall(const char* a) {
    (void)a;
    unsigned n = smp_online();
    if (n > proc_limit()) n = proc_limit();
    uart_print("[LOAD] "); uart_print_dec(n);
    uart_print(" equal burn processes for 3 s (one per online core); each runs on its own core, iteration counts match a single 'burn'\n");
    for (unsigned i = 0; i < n; i++) prog_start("burn");
}
static void cmd_starve(const char* a) { (void)a; uart_print("[TEST] realtime burner for 8 s: 'core' will starve, watchdog must fire (~5 s)...\n"); spawn_burners(1, 8, PRIO_REALTIME); }
static void cmd_3d(const char* a) { (void)a; prog_start("cube3d"); }
static void cmd_fb(const char* a) { (void)a; display_print_info(); }
static void cmd_hw(const char* a) { (void)a; platform_hw(uart_print); }
static void cmd_fpu(const char* a) { (void)a; uart_print("[FPU TEST] 3 fpu processes compute x87 + SSE + double in parallel; each reports its own verdict\n"); prog_start("fpu"); prog_start("fpu"); prog_start("fpu"); }
static void cmd_keys(const char* a) {
    (void)a;
    uint8_t b[64];
    unsigned n = input_raw_take(b, sizeof b);
    uart_print("[TEST] raw PS/2 keyboard bytes since the previous 'test keys' (press keys, then run again; make code, release = +0x80):\n ");
    for (unsigned i = 0; i < n; i++) {
        uart_putchar(' ');
        uart_putchar("0123456789ABCDEF"[b[i] >> 4]); uart_putchar("0123456789ABCDEF"[b[i] & 15]);
        if (b[i] == 0x0E) uart_print("(BkSp)");
        else if (b[i] == 0x8E) uart_print("(BkSp up)");
    }
    uart_print(n ? "\n" : " (none)\n");
}
static void cmd_threads(const char* a) { (void)a; uart_print("[TEST] program mt: 4 threads of one process, futex mutex, then exit with live threads\n"); prog_start("mt"); }
static void cmd_ipc(const char* a) { (void)a; uart_print("[TEST] two ipc processes: named ports (200 round trips), shared memory page, futex across processes\n"); prog_start("ipc"); prog_start("ipc"); }
static void cmd_isolation(const char* a) { (void)a; uart_print("[TEST] two demo processes write the same address: each must see only its own value\n"); prog_start("demo"); prog_start("demo"); }
static void cmd_atk(const char* a) {
    (void)a;
#if defined(__aarch64__)
    uart_print("[TEST] four hostile programs: kernel memory write, privileged instruction, kernel pointer to a syscall, jump into kernel code; each must be stopped, the kernel lives\n");
    prog_start("atk-mem"); prog_start("atk-priv"); prog_start("atk-sys"); prog_start("atk-jmp");
#else
    uart_print("[TEST] four hostile programs: kernel memory write, privileged instruction, kernel pointer to a syscall, corrupted GS; each must be stopped, the kernel lives\n");
    prog_start("atk-mem"); prog_start("atk-priv"); prog_start("atk-sys"); prog_start("atk-gs");
#endif
}
static void cmd_smp(const char* a) { (void)a; smp_describe(uart_print); }
static void cmd_smptest(const char* a) {
    (void)a;
    int r = smp_selftest_start();
    if (r > 0) { uart_print("[KIRA SMPTEST] started "); uart_print_dec((uint64_t)r); uart_print(" worker threads on the other cores; result will be printed when they finish.\n"); }
    else if (r == -1) uart_print("[KIRA SMPTEST] needs at least one started extra core (start QEMU with SMP=2 or more).\n");
    else if (r == -2) uart_print("[KIRA SMPTEST] LAPIC timer is not the tick source: other cores have no preemption.\n");
    else if (r == -3) uart_print("[KIRA SMPTEST] previous run is still in progress.\n");
    else uart_print("[KIRA SMPTEST] cannot create worker threads.\n");
}
static int word_is(const char* a, const char* w) {
    while (*w && *a == *w) { a++; w++; }
    return !*w && (!*a || *a == ' ');
}
static void cmd_smpcrash(const char* a) {
    while (a && *a == ' ') a++;
    unsigned mode = 9, cpu = 0;
    if (a && a[0] >= '0' && a[0] <= '9') {
        mode = 1;
        for (; *a >= '0' && *a <= '9'; a++) cpu = cpu * 10 + (unsigned)(*a - '0');
    } else if (a && word_is(a, "thread")) mode = 0;
    else if (a && word_is(a, "lock")) mode = 2;                  /* только полным словом: эта проверка останавливает систему */
    else if (a && word_is(a, "stack")) mode = 3;                 /* переполнение стека потока: тоже авария */
    if (mode == 9) {
        uart_print("usage: test crash N       core N fails (machine check): taken out of service, system continues\n");
        uart_print("       test crash thread  kernel thread faults: only that thread dies\n");
        uart_print("       test crash lock    fault under a spinlock: cannot be isolated, expected full panic\n");
        uart_print("       test crash stack   kernel thread overflows its stack: spare stack, clear report, expected full panic\n");
        return;
    }
    int r = smp_crash_test(mode, cpu);
    if (r == -1) uart_print("[KIRA SMPCRASH] no such online core.\n");
    else if (r == -3) uart_print("[KIRA SMPCRASH] that core does not run threads (no LAPIC tick: see the boot log, IRQ mode).\n");
    else if (r == -4) uart_print("[KIRA SMPCRASH] not available on this architecture yet (docs/HANDOFF.md, plan).\n");
    else if (r == -5) uart_print("[KIRA SMPCRASH] core failure is an SError on ARM64; this CPU cannot raise SError from software (no RAS error injection), so the path is not testable here.\n");
    else if (r < 0) uart_print("[KIRA SMPCRASH] cannot create the thread.\n");
    else uart_print("[KIRA SMPCRASH] crash thread started.\n");
}
static void cmd_plasma(const char* a) { (void)a; prog_start("plasma"); }
static void cmd_edit(const char* a) { (void)a; prog_start("edit"); }
/* Выключение и перезагрузка: сначала опыт и кэши дисков, потом - машина. */
static void power_prepare(void) {
    uart_print("сохраняю опыт и данные дисков...\n");
    experience_save_now();
    for (int i = 0; i < 4; i++) vfs_flush(i);
    int bad = blk_flush_all();
    if (bad) { uart_print("[KIRA DISK] write cache not flushed on "); uart_print_dec((uint64_t)bad); uart_print(" disk(s)\n"); }
}
static void cmd_random(const char* a) {
    (void)a;
    uint8_t b[16];
    int q = rng_get(b, sizeof b);
    uart_print("генератор случайных чисел: источники - "); uart_print(rng_sources());
    uart_print(q ? " (засеян аппаратным источником)\n" : " (аппаратного источника нет: только времена событий - для ключей слабее)\n");
    uart_print("проба: ");
    for (int i = 0; i < 16; i++) { uart_putchar("0123456789abcdef"[b[i] >> 4]); uart_putchar("0123456789abcdef"[b[i] & 15]); }
    uart_print("\n");
}
static void cmd_poweroff(const char* a) { (void)a; power_prepare(); platform_poweroff(); }
static void cmd_reboot(const char* a) { (void)a; power_prepare(); platform_reboot(); }
static void cmd_wasm(const char* a) {                       /* модуль WebAssembly - в интерпретаторе (Ring 3) */
    while (a && *a == ' ') a++;
    if (!a || *a != '/') { uart_print("wasm /путь/модуль.wasm: программа WebAssembly (одна для x86-64 и ARM64), модули - в /wasm\n"); return; }
    prog_request_file("wasm", a);
}
static void cmd_view(const char* a) {                       /* то же, что двойной щелчок в Проводнике */
    while (a && *a == ' ') a++;
    if (!a || *a != '/') { uart_print("view /путь/к/файлу: просмотр (текст, PNG, BMP, PPM, шестнадцатерично)\n"); return; }
    prog_request_file("view", a);
}
static void cmd_paint(const char* a) { (void)a; prog_start("paint"); }
static void cmd_exp(const char* a) { (void)a; experience_print(); }
static void cmd_save(const char* a) {
    (void)a;
    if (experience_save_now() == 0) uart_print("[EXPERIENCE] saved.\n"); else uart_print("[EXPERIENCE] not saved (no disk or write error).\n");
}
static void cmd_mem(const char* a) { (void)a; self_test_memory(); }
static void cmd_immune(const char* a) { (void)a; immune_demo(); }
static void cmd_crash_null(const char* a) { (void)a; uart_print("[TEST] reading address 0...\n"); (void)*(volatile char*)0; }
#if defined(__x86_64__)
static void cmd_crash_ud(const char* a) { (void)a; uart_print("[TEST] executing ud2...\n"); __asm__ volatile ("ud2"); }
static void cmd_crash_df(const char* a) {
    (void)a;
    uart_print("[TEST] corrupting RSP and raising #UD -> double fault expected...\n");
    __asm__ volatile ("xor %%rsp, %%rsp\n\tud2" : : : "memory");
}
#elif defined(__aarch64__)
static void cmd_crash_ud(const char* a) { (void)a; uart_print("[TEST] executing udf #0...\n"); __asm__ volatile ("udf #0"); }
static void cmd_crash_df(const char* a) {           /* у ARM64 нет отдельного стека двойной ошибки (IST) - это шаг плана */
    (void)a;
    uart_print("[TEST] ARM64: a fault on a corrupted kernel stack is not caught on a spare stack yet (plan) - not run.\n");
}
#else
static void cmd_crash_ud(const char* a) { (void)a; uart_print("[TEST] not on this host\n"); }
static void cmd_crash_df(const char* a) { (void)a; uart_print("[TEST] not on this host\n"); }
#endif
static void cmd_lspci(const char* a) { (void)a; pci_print(); }
static void cmd_disk(const char* a) { (void)a; blk_print(); }
static void cmd_run(const char* arg) {
    while (*arg == ' ') arg++;
    if (!*arg) { uart_print("использование: run имя   (программы лежат в /bin, список: progs)\n"); return; }
    char name[20]; unsigned n = 0;
    while (arg[n] && arg[n] != ' ' && n < sizeof name - 1) { name[n] = arg[n]; n++; }
    name[n] = 0;
    const char* rest = arg + n; while (*rest == ' ') rest++;
    int r = prog_run_args(name, rest);                       /* остаток строки - аргументы программы */
    if (r == -3) uart_print("[PROG] файл не принят как программа (см. сообщение выше)\n");
}
static void cmd_progs(const char* a) { (void)a; prog_list(); }
static void cmd_life(const char* a) { (void)a; life_print(); }
static void cmd_telemetry(const char* arg) {
    if (arg[0] == 'o' && arg[1] == 'n' && !arg[2]) mind_sys_set_telemetry(1);
    else if (arg[0] == 'o' && arg[1] == 'f' && arg[2] == 'f' && !arg[3]) mind_sys_set_telemetry(0);
    else if (arg[0]) { uart_print("использование: telemetry on | off\n"); return; }
    uart_print(mind_sys_telemetry() ? "телеметрия построчно на экране: включена\n" : "телеметрия построчно на экране: выключена (идёт только в serial; цифры видны в окне «Разум»)\n");
}

static int word_eq(const char* a, const char* w) {
    while (*w && *a == *w) { a++; w++; }
    return !*w && (!*a || *a == ' ');
}

static void cmd_usb(const char* a) {
    while (*a == ' ') a++;
    if (a[0] == 'a' && a[1] == 'l' && a[2] == 'l' && a[3] == 'o' && a[4] == 'w' && !a[5]) {
        usb_hid_allow_all();
        uart_print("отключённые Разумом клавиатуры подключаются снова и больше не задерживаются\n");
        return;
    }
    usb_print();
}

static void cmd_ai(const char* arg) {
    while (*arg == ' ') arg++;
    if (!*arg) { mind_sys_print(""); return; }
    if (word_eq(arg, "why")) { mind_sys_print("why"); return; }
    if (word_eq(arg, "norms")) { mind_sys_print("norms"); return; }
    if (word_eq(arg, "procs")) { mind_sys_print("procs"); return; }
    if (word_eq(arg, "model")) { mind_sys_print("model"); return; }
    if (word_eq(arg, "levers")) { mind_sys_print("levers"); return; }
    if (arg[0] == 'c' && arg[1] == 'a' && arg[2] == 'p' && (arg[3] == ' ' || !arg[3])) {
        const char* v = arg + 3; while (*v == ' ') v++;
        int n = 0; while (*v >= '0' && *v <= '9') n = n * 10 + (*v++ - '0');
        if (n < 10 || n > 100) {
            uart_print("предел загрузки ядер фоном сейчас "); uart_print_dec(mind_sys_core_cap());
            uart_print("%: когда экран опаздывает или система ждёт процессор, фон получает не больше этой доли всех ядер.\n"
                       "изменить: ai cap 10..100\n");
            return;
        }
        mind_sys_request_cap(n); uart_print("предел загрузки ядер фоном: "); uart_print_dec((uint64_t)n); uart_print("%\n");
        return;
    }
    if (word_eq(arg, "act")) { mind_sys_request_mode(1); uart_print("Разум: режим управления (применится в течение секунды)\n"); return; }
    if (word_eq(arg, "observe")) { mind_sys_request_mode(0); uart_print("Разум: режим наблюдения - решения только записываются, всё изменённое возвращается к обычному\n"); return; }
    if (arg[0] == 'f' && arg[1] == 'x' && arg[2] == ' ') {
        const char* v = arg + 3;
        while (*v == ' ') v++;
        if (word_eq(v, "auto")) { mind_sys_request_fx(MIND_FX_AUTO); uart_print("анимация фона: решает Разум\n"); return; }
        if (v[0] >= '0' && v[0] <= '3' && (!v[1] || v[1] == ' ')) { mind_sys_request_fx(v[0] - '0'); uart_print("анимация фона: задана вручную\n"); return; }
    }
    uart_print("использование: ai [why | norms | procs | model | levers | cap N | act | observe | fx auto|0..3]\n");
}

// Проверки системы - отдельной таблицей под командой test: в основном списке остаются рабочие команды.
// Тесты, которые останавливают систему (panic-*, starve), выполняются только полным именем и со словом yes.
typedef struct { const char* name; const char* help; void (*fn)(const char* arg); int halts; } test_t;
static const test_t tests[] = {
    { "smp",        "потоки на всех ядрах: расчёт, замок, вытеснение", cmd_smptest, 0 },
    { "crash",      "изоляция сбоев: crash thread | crash N (отказ ядра N) | crash lock (авария)", cmd_smpcrash, 0 },
    { "threads",    "потоки одного процесса на разных ядрах, мьютекс на futex, выход при живых потоках", cmd_threads, 0 },
    { "ipc",        "обмен между процессами: порты, разделяемая память, futex через общую память", cmd_ipc, 0 },
    { "fpu",        "состояние x87/SSE у трёх процессов", cmd_fpu, 0 },
    { "isolation",  "два процесса пишут по одному адресу: адресные пространства раздельны", cmd_isolation, 0 },
    { "atk",        "четыре враждебные программы: каждую останавливают, ядро живо", cmd_atk, 0 },
    { "mem",        "самопроверка кучи, PMM и страницы NULL", cmd_mem, 0 },
    { "keys",       "сырые байты клавиатуры с прошлого вызова (проверка двойных нажатий)", cmd_keys, 0 },
    { "immune",     "иммунная система на образцах", cmd_immune, 0 },
    { "starve",     "realtime-поток морит ядро -> watchdog (система остановится)", cmd_starve, 1 },
    { "panic-null", "чтение адреса 0 в потоке консоли -> авария (система остановится)", cmd_crash_null, 1 },
    { "panic-ud",   "недопустимая инструкция -> авария (система остановится)", cmd_crash_ud, 1 },
    { "panic-df",   "двойная ошибка на отдельном стеке -> авария (система остановится)", cmd_crash_df, 1 },
};
#define NTESTS (sizeof(tests) / sizeof(tests[0]))

static void cmd_test(const char* a) {
    while (*a == ' ') a++;
    if (!*a) {
        uart_print("проверки (test имя [аргументы]):\n");
        for (unsigned i = 0; i < NTESTS; i++) {
            uart_print("  "); uart_print(tests[i].name);
            unsigned pad = 12; for (const char* p = tests[i].name; *p && pad; p++) pad--;
            while (pad--) uart_putchar(' ');
            uart_print(tests[i].help); uart_print("\n");
        }
        return;
    }
    for (unsigned i = 0; i < NTESTS; i++) {
        unsigned n = 0; while (tests[i].name[n] && a[n] == tests[i].name[n]) n++;
        if (tests[i].name[n] || (a[n] && a[n] != ' ')) continue;
        const char* rest = a + n; while (*rest == ' ') rest++;
        if (tests[i].halts && !(rest[0] == 'y' && rest[1] == 'e' && rest[2] == 's' && !rest[3])) {
            uart_print("эта проверка ОСТАНАВЛИВАЕТ систему (несохранённое пропадёт). Подтверждение: test ");
            uart_print(tests[i].name); uart_print(" yes\n");
            return;
        }
        tests[i].fn(rest);
        return;
    }
    uart_print("нет такой проверки: "); uart_print(a); uart_print(" (список: test)\n");
}

static const cmd_t cmds[] = {
    { "help",      'h', "список команд", cmd_help },
    { "clear",     0,   "очистить терминал (то же: Ctrl+L)", cmd_clear },
    { "status",    's', "время, память, куча, организм, иммунитет, APIC", cmd_status },
    { "ps",        'p', "таблица процессов и потоков", cmd_ps },
    { "smp",       0,   "ядра: состояние, APIC ID, переключения, занятость", cmd_smp },
    { "hw",        0,   "профиль железа: процессор, ядра, ОЗУ, пределы для программ", cmd_hw },
    { "fb",        'f', "статистика экрана", cmd_fb },
    { "burn",      'l', "процесс-нагрузка на 3 с (вытесняющее планирование)", cmd_burn },
    { "burnall",   0,   "по процессу-нагрузке на каждое ядро, 3 с (сравни итерации с burn)", cmd_burnall },
    { "3d",        'g', "окно 3D (программа cube3d из /bin)", cmd_3d },
    { "plasma",    'a', "приложение Ring 3: плазма", cmd_plasma },
    { "edit",      0,   "приложение Ring 3: текстовый редактор (F2 - сохранить, F3 - другой файл, F4 - новый)", cmd_edit },
    { "random",    0,   "генератор случайных чисел: источники и проба", cmd_random },
    { "poweroff",  0,   "выключить машину (опыт и диски сохраняются; ACPI S5 / PSCI)", cmd_poweroff },
    { "reboot",    0,   "перезагрузить машину (опыт и диски сохраняются)", cmd_reboot },
    { "wasm",      0,   "wasm /путь.wasm: программа WebAssembly (один файл для x86-64 и ARM64; модули в /wasm)", cmd_wasm },
    { "view",      0,   "view /путь: просмотр файла (текст, PNG, BMP, PPM, двоичный) - как двойной щелчок в Проводнике", cmd_view },
    { "paint",     'b', "приложение Ring 3: холст (мышь, клавиши 1-8, c - очистить)", cmd_paint },
    { "run",       0,   "запустить программу с диска: run имя (/bin/имя, проверка иммунитетом)", cmd_run },
    { "progs",     0,   "программы в /bin и вердикт иммунитета", cmd_progs },
    { "ls",        0,   "список файлов: ls [путь]", fscmd_ls },
    { "cat",       0,   "содержимое файла: cat путь", fscmd_cat },
    { "write",     0,   "записать текст в файл: write путь текст", fscmd_write },
    { "mkdir",     0,   "создать каталог: mkdir путь", fscmd_mkdir },
    { "rm",        0,   "удалить файл или пустой каталог: rm путь", fscmd_rm },
    { "df",        0,   "место: системный диск и флешки", fscmd_df },
    { "cp",        0,   "копировать файл: cp откуда куда (флешка /usb0 <-> диск)", fscmd_cp },
    { "umount",    0,   "отключить флешку перед извлечением: umount usb0", fscmd_umount },
    { "format",    0,   "KiraFS на диске (стирает его): format ИМЯ yes; без слов - справка", fscmd_format },
    { "disk",      0,   "диски (NVMe, SATA, virtio, ATA) и какой из них системный", cmd_disk },
    { "lspci",     0,   "устройства на шине PCI", cmd_lspci },
    { "net",       0,   "сеть: карта, канал, адрес (DHCP), шлюз, счётчики", netcmd_net },
    { "ping",      0,   "ping адрес [число]: эхо ICMP, время ответа", netcmd_ping },
    { "sound",     0,   "звук: карта Intel HDA, кодек, путь к выходу", hdacmd_sound },
    { "beep",      0,   "beep [Гц] [мс]: тон через звуковую карту", hdacmd_beep },
    { "dns",       0,   "dns имя: адрес по DNS", netcmd_dns },
    { "http",      0,   "http адрес[:порт] [путь]: запрос по TCP, начало ответа", netcmd_http },
    { "exp",       'e', "опыт: выученная норма, запуски, поколение на диске", cmd_exp },
    { "save",      'v', "сохранить опыт на диск сейчас", cmd_save },
    { "life",      0,   "журнал организма из /var/life.log", cmd_life },
    { "ai",        'm', "Разум: здоровье, решения (ai why), нормы (ai norms), программы (ai procs), режим", cmd_ai },
    { "usb",       0,   "USB: дерево устройств, флешки; usb allow - вернуть отключённые клавиатуры", cmd_usb },
    { "telemetry", 0,   "telemetry on|off: построчная телеметрия на экране (по умолчанию только serial)", cmd_telemetry },
    { "test",      't', "проверки системы: test - список, test имя - запуск", cmd_test },
};
#define NCMDS (sizeof(cmds) / sizeof(cmds[0]))

static void cmd_help(const char* a) {
    (void)a;
    uart_print("команды (выполняются по Enter):\n");
    for (unsigned i = 0; i < NCMDS; i++) {
        uart_print("  "); uart_print(cmds[i].name);
        unsigned pad = 12; for (const char* p = cmds[i].name; *p; p++) pad--;
        if (cmds[i].alias) { uart_print(" ("); uart_putchar(cmds[i].alias); uart_print(")"); pad -= 4; }
        while ((int)pad-- > 0) uart_putchar(' ');
        uart_print(cmds[i].help); uart_print("\n");
    }
    uart_print("клавиши: Backspace, Ctrl+U стереть строку, Ctrl+C отмена, Ctrl+L очистить, стрелки вверх/вниз - история;\n"
               "         F11 - панель на весь экран, F10 - свернуть; размер панелей меняется уголком справа внизу.\n");
}

static void show_prompt(void) { uart_print(PROMPT); prompt_shown = 1; }

static int same(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void run_line(char* l) {
    while (*l == ' ') l++;
    if (!*l) return;
    char* arg = l;
    while (*arg && *arg != ' ') arg++;
    if (*arg) { *arg++ = 0; while (*arg == ' ') arg++; }
    for (unsigned i = 0; i < NCMDS; i++) {
        if (same(l, cmds[i].name) || (cmds[i].alias && l[0] == cmds[i].alias && !l[1] && cmds[i].alias != '?')) {
            cmds[i].fn(arg);
            return;
        }
    }
    if (l[0] == '?' && !l[1]) { cmd_help(arg); return; }
    if (platform_console_cmd(l, arg)) return;               /* команды только этой архитектуры */
    int r = prog_run_args(l, arg);                          /* как в оболочке: имя программы из /bin, дальше - аргументы */
    if (r == -1) { uart_print("неизвестная команда: "); uart_print(l); uart_print(" (help - список, progs - программы)\n"); }
    else if (r == -3) uart_print("[PROG] файл не принят как программа (см. сообщение выше)\n");
}

static void erase_echo(void) { while (line_len) { uart_print("\b \b"); line_len--; } }

static void set_line(const char* s) {
    erase_echo();
    while (*s && line_len < LINE_MAX - 1) { line[line_len++] = *s; uart_putchar(*s++); }
    line[line_len] = 0;
}

static void history_add(const char* l) {
    if (!*l) return;
    if (hist_n && same(hist[hist_n - 1], l)) return;
    if (hist_n == HIST_N) { for (unsigned i = 1; i < HIST_N; i++) { unsigned k = 0; while ((hist[i - 1][k] = hist[i][k])) k++; } hist_n--; }
    unsigned k = 0; while ((hist[hist_n][k] = l[k])) k++;
    hist_n++;
}

#define CK_UP   0x1001
#define CK_DOWN 0x1002
#define CK_CP   0x10000                  /* символ Unicode с клавиатуры (в строку - как UTF-8) */

/* Источник ввода: ASCII-символ, CK_UP/CK_DOWN или -1. UART, затем события клавиатуры. */
static int console_next_char(void) {
    int c = uart_getchar();
    if (c >= 0) return c;
    if (gfx_focus_pid()) return -1;      // клавиатура принадлежит окну приложения Ring 3
    input_event_t ev;
    while (input_pop(&ev)) {
        if (ev.type != INPUT_KEY_DOWN) continue;
        if (ev.key == KEY_UP) return CK_UP;
        if (ev.key == KEY_DOWN) return CK_DOWN;
        if (ev.ch && ev.ch < 128) return (int)ev.ch;
        if (ev.ch >= 0x80 && ev.ch < 0x800) return CK_CP | ev.ch;   /* кириллица (раскладка ЙЦУКЕН): код символа */
    }
    return -1;
}

#define REQ_N   4
#define REQ_LEN 64
static spinlock_t req_lock = SPINLOCK_INIT;
static char req_q[REQ_N][REQ_LEN];
static unsigned req_head, req_tail;

int console_request(const char* l) {
    uint64_t f = spin_lock_irqsave(&req_lock);
    if (req_head - req_tail >= REQ_N) { spin_unlock_irqrestore(&req_lock, f); return -1; }
    char* d = req_q[req_head % REQ_N];
    unsigned i = 0;
    while (l[i] && i < REQ_LEN - 1) { d[i] = l[i]; i++; }
    d[i] = 0;
    req_head++;
    spin_unlock_irqrestore(&req_lock, f);
    return 0;
}

/* Команды оболочки: недописанная строка пользователя убирается с экрана, команда печатается и выполняется,
   затем строка пользователя возвращается на место. */
static void run_requests(void) {
    while (req_tail != req_head) {
        char cmd[REQ_LEN];
        uint64_t f = spin_lock_irqsave(&req_lock);
        unsigned i = 0; const char* q = req_q[req_tail % REQ_N];
        while (q[i]) { cmd[i] = q[i]; i++; }
        cmd[i] = 0;
        req_tail++;
        spin_unlock_irqrestore(&req_lock, f);
        unsigned keep = line_len;
        for (unsigned k = 0; k < keep; k++) uart_print("\b \b");
        uart_print(cmd); uart_putchar('\n');
        history_add(cmd);
        char tmp[REQ_LEN]; for (unsigned k = 0; k <= i; k++) tmp[k] = cmd[k];
        run_line(tmp);
        hist_pos = hist_n;
        show_prompt();
        for (unsigned k = 0; k < keep; k++) uart_putchar(line[k]);
    }
}

void console_poll(void) {
    static int last_cr;
    if (!prompt_shown) { uart_print("\n"); show_prompt(); }
    run_requests();
    int c, budget = 512;                       /* за один вызов - не больше 512 знаков: основной поток всегда идёт дальше */
    while (budget-- > 0 && (c = console_next_char()) >= 0) {
        event_count++;
        if (c == '\n' && last_cr) { last_cr = 0; continue; }       // пара CR LF от терминала - одна команда
        last_cr = (c == '\r');
        if (c == '\r' || c == '\n') {
            uart_putchar('\n');
            line[line_len] = 0;
            history_add(line);
            run_line(line);
            line_len = 0; hist_pos = hist_n;
            show_prompt();
        } else if (c == 8 || c == 127) {
            if (line_len) {                                           /* символ целиком: UTF-8 - несколько байт */
                while (line_len && ((unsigned char)line[line_len - 1] & 0xC0) == 0x80) line_len--;
                if (line_len) line_len--;
                uart_print("\b \b");
            }
        } else if (c == 21) {                                         // Ctrl+U
            erase_echo();
        } else if (c == 3) {                                          // Ctrl+C
            uart_print("^C\n"); line_len = 0; hist_pos = hist_n; show_prompt();
        } else if (c == 12) {                                         // Ctrl+L: очистить, строку оставить
            cmd_clear(0);
            uart_print(PROMPT);
            for (unsigned i = 0; i < line_len; i++) uart_putchar(line[i]);
        } else if (c == CK_UP) {
            if (hist_pos > 0) { hist_pos--; set_line(hist[hist_pos]); }
        } else if (c == CK_DOWN) {
            if (hist_pos + 1 < hist_n) { hist_pos++; set_line(hist[hist_pos]); }
            else { hist_pos = hist_n; erase_echo(); }
        } else if (c >= 0x20 && c < 0x7F) {
            if (line_len < LINE_MAX - 1) { line[line_len++] = (char)c; uart_putchar((char)c); }
        } else if (c & CK_CP) {                                       /* с клавиатуры: два байта UTF-8 */
            unsigned cp = (unsigned)c & 0x7FF;
            if (line_len + 2 < LINE_MAX) {
                char u[3] = { (char)(0xC0 | cp >> 6), (char)(0x80 | (cp & 0x3F)), 0 };
                line[line_len++] = u[0]; line[line_len++] = u[1];
                uart_print(u);
            }
        } else if (c >= 0x80 && c <= 0xF4 && c != 0xC0 && c != 0xC1) {   /* из порта - байт UTF-8 (0xFF «пустого» порта - нет) */
            if (line_len < LINE_MAX - 1) { line[line_len++] = (char)c; uart_putchar((char)c); }
        }
    }
}
