/* Kira OS на ARM64: подъём платформы. Всё железо берётся из дерева устройств - адресов в коде нет:
   порт PL011, ОЗУ (все узлы memory), контроллер прерываний GICv2 или GICv3, общий таймер ARM, ядра и способ их
   запуска (PSCI через hvc или smc). Включается MMU с тождественной картой (ОЗУ - обычная кэшируемая память,
   остальное - устройства без исполнения), каждое ядро получает свой тик и проверяется.
   Это первый этап переноса: общие части ядра (планировщик, Разум, файловые системы) подключаются следующими. */
#include <stdint.h>
#include "fdt.h"

#define TIMER_HZ 500
#define MAX_RAM 16

extern char __kernel_start[], __kernel_end[];
extern void secondary_entry(void);

static inline void mmio_w32(uint64_t a, uint32_t v) { *(volatile uint32_t*)(uintptr_t)a = v; }
static inline uint32_t mmio_r32(uint64_t a) { return *(volatile uint32_t*)(uintptr_t)a; }
static inline uint64_t mmio_r64(uint64_t a) { return *(volatile uint64_t*)(uintptr_t)a; }
#define SYSREG_R(name) ({ uint64_t _v; __asm__ volatile ("mrs %0, " #name : "=r"(_v)); _v; })
#define SYSREG_W(name, v) __asm__ volatile ("msr " #name ", %0" : : "r"((uint64_t)(v)))
static inline void isb(void) { __asm__ volatile ("isb" ::: "memory"); }
static inline void dsb(void) { __asm__ volatile ("dsb sy" ::: "memory"); }

/* ---------------- порт PL011 ---------------- */
/* Отладка подъёма: сборка с -DKIRA_EARLY_UART=адрес печатает и до разбора дерева (только для стенда). */
#ifdef KIRA_EARLY_UART
static uint64_t uart = KIRA_EARLY_UART;
#else
static uint64_t uart;
#endif
static volatile int mmu_on;
static volatile uint32_t print_lock;

static void uart_putc(char c) {
    if (!uart) return;
    for (int i = 0; i < 1000000 && (mmio_r32(uart + 0x18) & (1u << 5)); i++) { }   /* FR.TXFF: очередь полна */
    mmio_w32(uart, (uint8_t)c);
}
static int uart_getc(void) {
    if (!uart || (mmio_r32(uart + 0x18) & (1u << 4))) return -1;                    /* FR.RXFE: пусто */
    return (int)(mmio_r32(uart) & 0xFF);
}
static void lock(void) { if (mmu_on) while (__atomic_exchange_n(&print_lock, 1, __ATOMIC_ACQUIRE)) __asm__ volatile ("yield"); }
static void unlock(void) { if (mmu_on) __atomic_store_n(&print_lock, 0, __ATOMIC_RELEASE); }
static void puts_raw(const char* s) { for (; *s; s++) { if (*s == '\n') uart_putc('\r'); uart_putc(*s); } }
static void put_dec_raw(uint64_t v) { char b[21]; int i = 20; b[i] = 0; do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v); puts_raw(&b[i]); }
static void put_hex_raw(uint64_t v) { char b[19]; b[0] = '0'; b[1] = 'x'; for (int i = 0; i < 16; i++) b[2 + i] = "0123456789ABCDEF"[(v >> (60 - 4 * i)) & 15]; b[18] = 0; puts_raw(b); }
/* Строка целиком под замком: строки разных ядер не перемешиваются. Формат: %s %u %x. */
static void kprintf(const char* f, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, f);
    lock();
    for (; *f; f++) {
        if (*f != '%') { if (*f == '\n') uart_putc('\r'); uart_putc(*f); continue; }
        f++;
        if (*f == 's') puts_raw(__builtin_va_arg(ap, const char*));
        else if (*f == 'u') put_dec_raw(__builtin_va_arg(ap, uint64_t));
        else if (*f == 'x') put_hex_raw(__builtin_va_arg(ap, uint64_t));
        else if (*f == '%') uart_putc('%');
    }
    unlock();
    __builtin_va_end(ap);
}

/* ---------------- память ---------------- */
static struct { uint64_t base, size; } ram[MAX_RAM];
static unsigned nram;
static uint64_t bump, bump_end;

static void* palloc(uint64_t bytes, uint64_t align) {          /* страницы для таблиц и стеков - за ядром */
    uint64_t a = (bump + align - 1) & ~(align - 1);
    if (a + bytes > bump_end) return 0;
    bump = a + bytes;
    for (uint64_t i = 0; i < bytes; i += 8) *(volatile uint64_t*)(uintptr_t)(a + i) = 0;
    /* Вторичное ядро пишет в свой стек ещё до включения MMU (мимо кэша): строки, обнулённые здесь через кэш,
       выталкиваются в память и сбрасываются, иначе их вытеснение позже затёрло бы его данные. */
    if (mmu_on) {
        for (uint64_t x = a & ~63ull; x < a + bytes; x += 64) __asm__ volatile ("dc civac, %0" : : "r"(x) : "memory");
        dsb();
    }
    return (void*)(uintptr_t)a;
}

static int in_ram(uint64_t a, uint64_t len) {
    for (unsigned i = 0; i < nram; i++) if (a >= ram[i].base && a + len <= ram[i].base + ram[i].size) return 1;
    return 0;
}
static int touches_ram(uint64_t a, uint64_t len) {
    for (unsigned i = 0; i < nram; i++) if (a < ram[i].base + ram[i].size && ram[i].base < a + len) return 1;
    return 0;
}

/* ---------------- MMU: тождественная карта, 4 КБ гранула, 48 бит ----------------
   MAIR: 0 - устройство nGnRnE, 1 - устройство nGnRE, 2 - обычная память с обратной записью. ОЗУ - блоки по 1 ГБ,
   а где ОЗУ занимает часть гигабайта - по 2 МБ (обращение «наугад» к несуществующей обычной памяти на настоящем
   железе даёт SError, поэтому дыры остаются устройствами без исполнения). */
#define PTE_VALID   1ull
#define PTE_TABLE   3ull
#define PTE_BLOCK   1ull
#define PTE_AF      (1ull << 10)
#define PTE_SH_IN   (3ull << 8)
#define PTE_NORMAL  ((2ull << 2) | PTE_AF | PTE_SH_IN)
#define PTE_DEVICE  ((1ull << 2) | PTE_AF | (1ull << 53) | (1ull << 54))
static uint64_t* l0;

static uint64_t* next_table(uint64_t* t, unsigned idx) {
    if (!(t[idx] & PTE_VALID)) {
        uint64_t* n = (uint64_t*)palloc(4096, 4096);
        if (!n) return 0;
        t[idx] = (uint64_t)(uintptr_t)n | PTE_TABLE;
    } else if ((t[idx] & 3) != PTE_TABLE) return 0;
    return (uint64_t*)(uintptr_t)(t[idx] & 0x0000FFFFFFFFF000ull);
}

/* Отобразить гигабайт с адресом gb (1 ГБ-выровненный). */
static int map_gb(uint64_t gb) {
    uint64_t* l1 = next_table(l0, (unsigned)(gb >> 39) & 511);
    if (!l1) return -1;
    unsigned i1 = (unsigned)(gb >> 30) & 511;
    if (l1[i1] & PTE_VALID) return 0;
    if (in_ram(gb, 1ull << 30)) { l1[i1] = gb | PTE_NORMAL | PTE_BLOCK; return 0; }
    if (!touches_ram(gb, 1ull << 30)) { l1[i1] = gb | PTE_DEVICE | PTE_BLOCK; return 0; }
    uint64_t* l2 = next_table(l1, i1);
    if (!l2) return -1;
    for (unsigned i = 0; i < 512; i++) {
        uint64_t a = gb + ((uint64_t)i << 21);
        l2[i] = a | (in_ram(a, 1ull << 21) ? PTE_NORMAL : PTE_DEVICE) | PTE_BLOCK;
    }
    return 0;
}

static uint64_t tcr_val;
static void mmu_enable_this_cpu(void) {
    SYSREG_W(mair_el1, 0x00ull | (0x04ull << 8) | (0xFFull << 16));
    SYSREG_W(tcr_el1, tcr_val);
    SYSREG_W(ttbr0_el1, (uint64_t)(uintptr_t)l0);
    dsb(); isb();
    __asm__ volatile ("tlbi vmalle1\n dsb ish\n isb" ::: "memory");
    uint64_t sctlr = SYSREG_R(sctlr_el1);
    sctlr |= (1ull << 0) | (1ull << 2) | (1ull << 12);         /* M, C, I */
    sctlr &= ~(1ull << 1);                                       /* без проверки выравнивания */
    SYSREG_W(sctlr_el1, sctlr);
    isb();
}

static int mmu_init(uint64_t top) {
    l0 = (uint64_t*)palloc(4096, 4096);
    if (!l0) return -1;
    uint64_t parange = SYSREG_R(id_aa64mmfr0_el1) & 0xF;
    static const unsigned pa_bits[] = { 32, 36, 40, 42, 44, 48, 52 };
    unsigned bits = parange < 7 ? pa_bits[parange] : 48;
    if (bits > 48) { bits = 48; parange = 5; }
    if (top > (1ull << bits)) top = 1ull << bits;
    for (uint64_t gb = 0; gb < top; gb += 1ull << 30) if (map_gb(gb)) return -1;
    tcr_val = 16ull | (1ull << 8) | (1ull << 10) | (3ull << 12) | (1ull << 23) | (parange << 32);
    mmu_enable_this_cpu();
    mmu_on = 1;
    return (int)bits;
}

/* ---------------- GIC ---------------- */
static int gic_v3;
static uint64_t gicd, gicc, gicr_base, gicr_size;
static unsigned timer_intid;

#define ICC_SRE_EL1     S3_0_C12_C12_5
#define ICC_PMR_EL1     S3_0_C4_C6_0
#define ICC_IGRPEN1_EL1 S3_0_C12_C12_7
#define ICC_IAR1_EL1    S3_0_C12_C12_0
#define ICC_EOIR1_EL1   S3_0_C12_C12_1
#define XSTR(x) #x
#define SREG_R(n) ({ uint64_t _v; __asm__ volatile ("mrs %0, " XSTR(n) : "=r"(_v)); _v; })
#define SREG_W(n, v) __asm__ volatile ("msr " XSTR(n) ", %0" : : "r"((uint64_t)(v)))

static void gic_dist_init(void) {
    if (gic_v3) {
        mmio_w32(gicd, (1u << 4) | (1u << 1));                  /* GICD_CTLR: ARE_NS, EnableGrp1NS */
        for (int i = 0; i < 100000 && (mmio_r32(gicd) & (1u << 31)); i++) { }    /* RWP */
    } else {
        mmio_w32(gicd, 1);                                       /* GICD_CTLR: Enable */
    }
}

/* Редистрибьютор этого ядра (GICv3): по сродству в GICR_TYPER. */
static uint64_t my_gicr(void) {
    uint64_t mp = SYSREG_R(mpidr_el1);
    uint32_t aff = (uint32_t)(((mp >> 32) & 0xFF) << 24 | (mp & 0xFFFFFF));
    for (uint64_t r = gicr_base; r < gicr_base + gicr_size; ) {
        uint64_t typer = mmio_r64(r + 0x8);
        if ((uint32_t)(typer >> 32) == aff) return r;
        if (typer & (1u << 4)) break;                            /* Last */
        r += (typer & (1u << 1)) ? 0x40000 : 0x20000;           /* VLPIS: 4 кадра, иначе 2 */
    }
    return 0;
}

static int gic_cpu_init(void) {
    if (gic_v3) {
        uint64_t r = my_gicr();
        if (!r) return -1;
        mmio_w32(r + 0x14, mmio_r32(r + 0x14) & ~(1u << 1));     /* GICR_WAKER.ProcessorSleep = 0 */
        for (int i = 0; i < 1000000 && (mmio_r32(r + 0x14) & (1u << 2)); i++) { }   /* ChildrenAsleep */
        uint64_t sgi = r + 0x10000;
        mmio_w32(sgi + 0x80, mmio_r32(sgi + 0x80) | (1u << timer_intid));   /* IGROUPR0: группа 1 */
        *(volatile uint8_t*)(uintptr_t)(sgi + 0x400 + timer_intid) = 0x80;  /* приоритет */
        mmio_w32(sgi + 0x100, 1u << timer_intid);                           /* ISENABLER0 */
        SREG_W(ICC_SRE_EL1, SREG_R(ICC_SRE_EL1) | 1);
        isb();
        SREG_W(ICC_PMR_EL1, 0xFF);
        SREG_W(ICC_IGRPEN1_EL1, 1);
        isb();
    } else {
        *(volatile uint8_t*)(uintptr_t)(gicd + 0x400 + timer_intid) = 0x80;   /* GICD_IPRIORITYR (банк ядра) */
        mmio_w32(gicd + 0x100, 1u << timer_intid);                            /* GICD_ISENABLER0 */
        mmio_w32(gicc + 0x4, 0xFF);                                          /* GICC_PMR */
        mmio_w32(gicc, 1);                                                   /* GICC_CTLR */
    }
    return 0;
}

static uint32_t gic_ack(void) { return gic_v3 ? (uint32_t)SREG_R(ICC_IAR1_EL1) : mmio_r32(gicc + 0xC); }
static void gic_eoi(uint32_t iar) { if (gic_v3) SREG_W(ICC_EOIR1_EL1, iar); else mmio_w32(gicc + 0x10, iar); }

/* ---------------- ядра и таймер ---------------- */
#define CPU_MAX 256
typedef struct { uint64_t mpidr; volatile uint64_t ticks; volatile uint32_t online; uint32_t gic_ok; } cpu_t;
static cpu_t cpus[CPU_MAX];
static unsigned ncpu;
static uint64_t cnt_freq, tick_cycles;

static unsigned my_cpu(void) {
    uint64_t mp = SYSREG_R(mpidr_el1) & 0xFF00FFFFFFull;
    for (unsigned i = 0; i < ncpu; i++) if (cpus[i].mpidr == mp) return i;
    return 0;
}
static uint64_t now_cnt(void) { isb(); return SYSREG_R(cntvct_el0); }

static void timer_start(void) {
    SYSREG_W(cntv_tval_el0, tick_cycles);
    SYSREG_W(cntv_ctl_el0, 1);                                   /* ENABLE, прерывание не замаскировано */
}

static const char* ec_name(unsigned ec) {
    switch (ec) {
    case 0x15: return "SVC";
    case 0x20: case 0x21: return "instruction abort";
    case 0x24: case 0x25: return "data abort";
    case 0x22: return "PC alignment";
    case 0x26: return "SP alignment";
    case 0x00: return "unknown instruction";
    case 0x3C: return "BRK";
    default: return "other";
    }
}

/* Проба адреса (адрес дерева в x0 не обязан быть годным: часть загрузчиков оставляет там мусор): ошибка доступа во
   время пробы не авария - команда пропускается, проба считается неудачной. */
static volatile int probing, probe_failed;

void arm_exception(uint64_t* frame, uint64_t kind) {
    if ((kind == 0 || kind == 4) && probing) {
        unsigned ec0 = (unsigned)(frame[33] >> 26) & 0x3F;
        if (ec0 == 0x24 || ec0 == 0x25) { probe_failed = 1; frame[31] += 4; return; }
    }
    if (kind == 1 || kind == 5 || kind == 9) {                   /* IRQ */
        uint32_t iar = gic_ack();
        uint32_t id = iar & 0xFFFFFF;
        if (id >= 1020 && id <= 1023) return;                    /* ложное */
        if (id == timer_intid) {
            SYSREG_W(cntv_tval_el0, tick_cycles);
            cpus[my_cpu()].ticks++;
        }
        gic_eoi(iar);
        return;
    }
    uint64_t esr = frame[33], elr = frame[31], far = frame[34];
    unsigned ec = (unsigned)(esr >> 26) & 0x3F;
    mmu_on = 0;                                                  /* авария: печатать без замка */
    kprintf("\n[KIRA ARM64] exception on core %u: %s (class %x, vector %u), ESR %x\n", (uint64_t)my_cpu(), ec_name(ec), (uint64_t)ec, kind, esr);
    kprintf("[KIRA ARM64] ELR %x  FAR %x  SPSR %x  LR %x  SP %x\n", elr, far, frame[32], frame[30], (uint64_t)(uintptr_t)(frame + 36));
    kprintf("[KIRA ARM64] Halting this core.\n");
    for (;;) __asm__ volatile ("msr daifset, #0xf\n wfi");
}

/* ---------------- PSCI ---------------- */
static int psci_hvc = -1;                                        /* -1 - нет PSCI */
static int64_t psci_call(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3) {
    register uint64_t x0 __asm__("x0") = fn, x1 __asm__("x1") = a1, x2 __asm__("x2") = a2, x3 __asm__("x3") = a3;
    if (psci_hvc) __asm__ volatile ("hvc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    else __asm__ volatile ("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
    return (int64_t)x0;
}

void arm_secondary_main(void) {
    mmu_enable_this_cpu();
    unsigned me = my_cpu();
    cpus[me].gic_ok = gic_cpu_init() == 0;
    timer_start();
    __atomic_store_n(&cpus[me].online, 1, __ATOMIC_RELEASE);
    __asm__ volatile ("msr daifclr, #2");
    for (;;) __asm__ volatile ("wfi");
}

/* ---------------- консоль ---------------- */
static void cmd(const char* l) {
    if (l[0] == 's' && l[1] == 'm' && l[2] == 'p' && !l[3]) {
        for (unsigned i = 0; i < ncpu; i++)
            kprintf("  core %u: MPIDR %x, %s, ticks %u\n", (uint64_t)i, cpus[i].mpidr, cpus[i].online ? "online" : "OFF", cpus[i].ticks);
    } else if (l[0] == 'm' && l[1] == 'e' && l[2] == 'm' && !l[3]) {
        for (unsigned i = 0; i < nram; i++) kprintf("  RAM %x .. %x (%u MB)\n", ram[i].base, ram[i].base + ram[i].size, ram[i].size >> 20);
        kprintf("  kernel %x .. %x, early allocations up to %x\n", (uint64_t)(uintptr_t)__kernel_start, (uint64_t)(uintptr_t)__kernel_end, bump);
    } else if (l[0] == 'f' && l[1] == 'a' && l[2] == 'u' && l[3] == 'l' && l[4] == 't' && !l[5]) {
        kprintf("reading an address outside the map: the exception vectors must report it\n");
        (void)*(volatile uint64_t*)(uintptr_t)0x0000F00000000000ull;
    } else if (l[0]) {
        kprintf("commands: smp, mem, fault\n");
    }
}

/* ---------------- вход ---------------- */
static const void* find_fdt(uint64_t x0) {
    if (!x0 || (x0 & 7)) return 0;
    probing = 1; probe_failed = 0;
    int ok = *(volatile uint32_t*)(uintptr_t)x0 == 0xEDFE0DD0u;   /* FDT_MAGIC в памяти (большой порядок байт) */
    if (ok && !probe_failed) ok = fdt_valid((const void*)(uintptr_t)x0, 0);
    probing = 0;
    return ok && !probe_failed ? (const void*)(uintptr_t)x0 : 0;
}

void arm_main(uint64_t dtb, uint64_t entry_el) {
#ifdef KIRA_EARLY_UART
    kprintf("[early] arm_main x0=%x EL%u\n", dtb, (SYSREG_R(CurrentEL) >> 2) & 3);
#endif
    const void* fdt = find_fdt(dtb);
    /* QEMU кладёт дерево в начало ОЗУ, когда ядро - не образ Linux; адрес начала ОЗУ - ниже ядра */
    if (!fdt) {
        uint64_t k = (uint64_t)(uintptr_t)__kernel_start;
        for (uint64_t a = k & ~0x3FFFFFFFull; a < k && !fdt; a += 0x200000) fdt = find_fdt(a);
    }
#ifdef KIRA_EARLY_UART
    { uint64_t k = (uint64_t)(uintptr_t)__kernel_start & ~0x3FFFFFFFull;
      kprintf("[early] fdt=%x, word at RAM base %x = %x\n", (uint64_t)(uintptr_t)fdt, k, (uint64_t)*(volatile uint32_t*)(uintptr_t)k); }
#endif
    if (!fdt) for (;;) __asm__ volatile ("wfe");                 /* без дерева нет даже адреса порта */

    int u = fdt_find_compatible(fdt, -1, "arm,pl011");
    uint64_t sz;
#ifdef KIRA_EARLY_UART
    kprintf("[early] pl011 node %x\n", (uint64_t)(int64_t)u);
#endif
    if (u >= 0) fdt_reg(fdt, u, 0, &uart, &sz);
    kprintf("\n[KIRA ARM64] Kira OS on ARM64: device tree at %x (%u bytes), console PL011 at %x\n",
            (uint64_t)(uintptr_t)fdt, (uint64_t)fdt_total_size(fdt), uart);
    kprintf("[KIRA ARM64] entered at EL%u, running at EL%u\n", entry_el & 3, (SYSREG_R(CurrentEL) >> 2) & 3);

    /* ОЗУ: все узлы device_type = memory, все пары reg */
    uint64_t top = 4ull << 30;
    int depth = 0;
    for (int n = fdt_root(fdt); n >= 0; n = fdt_next_node(fdt, n, &depth)) {
        uint32_t len; const char* t = (const char*)fdt_prop(fdt, n, "device_type", &len);
        if (!t || len < 7 || __builtin_memcmp(t, "memory", 7)) continue;
        uint64_t a, s;
        for (unsigned k = 0; nram < MAX_RAM && fdt_reg(fdt, n, k, &a, &s) == 0; k++) if (s) {
            ram[nram].base = a; ram[nram].size = s; nram++;
            if (a + s > top) top = a + s;
        }
    }
    uint64_t total = 0; for (unsigned i = 0; i < nram; i++) total += ram[i].size;
    bump = (uint64_t)(uintptr_t)__kernel_end;
    uint64_t fdt_end = (uint64_t)(uintptr_t)fdt + fdt_total_size(fdt);
    if ((uint64_t)(uintptr_t)fdt >= bump && fdt_end > bump) bump = fdt_end;   /* дерево за ядром - не затирать */
    bump_end = bump;
    for (unsigned i = 0; i < nram; i++) if (bump >= ram[i].base && bump < ram[i].base + ram[i].size) bump_end = ram[i].base + ram[i].size;
    kprintf("[KIRA ARM64] RAM: %u MB in %u region(s)\n", total >> 20, (uint64_t)nram);

    int bits = mmu_init(top);
    if (bits < 0) { kprintf("[KIRA ARM64] FATAL: cannot build page tables\n"); for (;;) __asm__ volatile ("wfe"); }
    kprintf("[KIRA ARM64] MMU on: identity map up to %x, %u-bit physical addresses, RAM cached, devices non-executable\n", top, (uint64_t)bits);

    /* контроллер прерываний */
    int g = fdt_find_compatible(fdt, -1, "arm,gic-v3");
    if (g >= 0) {
        gic_v3 = 1;
        fdt_reg(fdt, g, 0, &gicd, &sz);
        fdt_reg(fdt, g, 1, &gicr_base, &gicr_size);
    } else {
        static const char* const v2[] = { "arm,cortex-a15-gic", "arm,gic-400", "arm,cortex-a9-gic", "arm,cortex-a7-gic" };
        for (unsigned i = 0; i < 4 && g < 0; i++) g = fdt_find_compatible(fdt, -1, v2[i]);
        if (g < 0) { kprintf("[KIRA ARM64] FATAL: no supported interrupt controller in the device tree\n"); for (;;) __asm__ volatile ("wfe"); }
        fdt_reg(fdt, g, 0, &gicd, &sz);
        fdt_reg(fdt, g, 1, &gicc, &sz);
    }
    /* таймер: прерывания в порядке secure phys, phys, virt, hyp; берём виртуальный */
    int tn = fdt_find_compatible(fdt, -1, "arm,armv8-timer");
    if (tn < 0) tn = fdt_find_compatible(fdt, -1, "arm,armv7-timer");
    uint32_t ilen = 0;
    const uint8_t* irq = tn >= 0 ? (const uint8_t*)fdt_prop(fdt, tn, "interrupts", &ilen) : 0;
    timer_intid = (irq && ilen >= 36) ? 16 + fdt_u32(irq + 24 + 4) : 27;
    cnt_freq = SYSREG_R(cntfrq_el0);
    tick_cycles = cnt_freq / TIMER_HZ;
    kprintf("[KIRA ARM64] interrupt controller GICv%u: distributor %x, %s %x; generic timer %u Hz, virtual timer INTID %u\n",
            (uint64_t)(gic_v3 ? 3 : 2), gicd, gic_v3 ? "redistributors" : "CPU interface", gic_v3 ? gicr_base : gicc, cnt_freq, (uint64_t)timer_intid);

    /* ядра */
    int cn = fdt_find_path(fdt, "/cpus");
    if (cn >= 0) {
        int d = 0;
        for (int c = fdt_next_node(fdt, cn, &d); c >= 0 && d > 0; c = fdt_next_node(fdt, c, &d)) {
            if (d != 1) continue;
            uint32_t len; const char* t = (const char*)fdt_prop(fdt, c, "device_type", &len);
            uint64_t a;
            if (!t || len < 4 || __builtin_memcmp(t, "cpu", 4) || fdt_reg(fdt, c, 0, &a, 0) || ncpu >= CPU_MAX) continue;
            cpus[ncpu++].mpidr = a;
        }
    }
    if (!ncpu) { cpus[0].mpidr = SYSREG_R(mpidr_el1) & 0xFF00FFFFFFull; ncpu = 1; }
    unsigned me = my_cpu();
    int ps = fdt_find_compatible(fdt, -1, "arm,psci-1.0");
    if (ps < 0) ps = fdt_find_compatible(fdt, -1, "arm,psci-0.2");
    if (ps < 0) ps = fdt_find_compatible(fdt, -1, "arm,psci");
    if (ps >= 0) {
        uint32_t len; const char* m = (const char*)fdt_prop(fdt, ps, "method", &len);
        if (m && len >= 3) psci_hvc = m[0] == 'h';
    }

    gic_dist_init();
    cpus[me].gic_ok = gic_cpu_init() == 0;
    timer_start();
    cpus[me].online = 1;
    __asm__ volatile ("msr daifclr, #2");

    /* проверка тика: 200 мс по счётчику */
    uint64_t t0 = now_cnt(), k0 = cpus[me].ticks;
    while (now_cnt() - t0 < cnt_freq / 5) __asm__ volatile ("wfi");
    uint64_t got = cpus[me].ticks - k0;
    kprintf("[KIRA ARM64] generic timer verified: %u ticks in 200 ms (expected %u)\n", got, (uint64_t)(TIMER_HZ / 5));

    /* остальные ядра через PSCI CPU_ON */
    unsigned started = 1;
    if (psci_hvc >= 0) {
        for (unsigned i = 0; i < ncpu; i++) {
            if (i == me) continue;
            uint8_t* st = (uint8_t*)palloc(16384, 16);
            if (!st) { kprintf("[KIRA ARM64] no memory for the stack of core %u\n", (uint64_t)i); break; }
            int64_t r = psci_call(0xC4000003ull, cpus[i].mpidr, (uint64_t)(uintptr_t)secondary_entry, (uint64_t)(uintptr_t)(st + 16384));
            if (r != 0) { kprintf("[KIRA ARM64] PSCI CPU_ON for core %u (MPIDR %x) failed: %u\n", (uint64_t)i, cpus[i].mpidr, (uint64_t)-r); continue; }
            uint64_t w = now_cnt();
            while (!__atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE) && now_cnt() - w < cnt_freq) { }
            if (cpus[i].online) started++;
            else kprintf("[KIRA ARM64] core %u did not come up within 1 s\n", (uint64_t)i);
        }
    }
    kprintf("[KIRA ARM64] Processors online: %u of %u (%s)\n", (uint64_t)started, (uint64_t)ncpu,
            psci_hvc < 0 ? "no PSCI in the device tree" : psci_hvc ? "PSCI via hvc" : "PSCI via smc");
    /* каждое ядро тикает своим таймером */
    t0 = now_cnt();
    while (now_cnt() - t0 < cnt_freq / 5) __asm__ volatile ("wfi");
    unsigned ticking = 0;
    for (unsigned i = 0; i < ncpu; i++) if (cpus[i].online && cpus[i].ticks > 0 && cpus[i].gic_ok) ticking++;
    kprintf("[KIRA ARM64] cores with a working timer interrupt: %u of %u\n", (uint64_t)ticking, (uint64_t)started);
    kprintf("[KIRA ARM64] Ready (platform layer). Commands: smp, mem, fault\n");

    char line[64]; unsigned n = 0;
    kprintf("kira-arm64> ");
    for (;;) {
        int c = uart_getc();
        if (c < 0) { __asm__ volatile ("wfi"); continue; }
        if (c == '\r' || c == '\n') {
            line[n] = 0; kprintf("\n"); cmd(line); n = 0; kprintf("kira-arm64> ");
        } else if ((c == 8 || c == 127) && n) { n--; kprintf("\b \b"); }
        else if (c >= 32 && n + 1 < sizeof line) { line[n++] = (char)c; char e[2] = { (char)c, 0 }; kprintf("%s", e); }
    }
}
