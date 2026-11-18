/* x86-64: сон S3 по ACPI. Уровень 2 (arch). Порядок действий сверху (устройства и потоки усыпляет sleep.c уровня 6, здесь - процессоры и машина):
     пакет \_S3 (интерпретатор AML, иначе поиск в DSDT) -> режим ACPI (SCI_EN) -> \_PTS/\_GTS -> \_PRW: источники пробуждения ->
     остальные ядра встают (каждое сохраняет своё состояние) -> вектор пробуждения в FACS -> состояние загрузочного ядра -> SLP_TYP|SLP_EN в PM1a/PM1b_CNT.
   Пробуждение: прошивка передаёт управление на вектор (физический адрес трамплина ap_tramp.s, реальный режим) -> защищённый -> длинный режим ->
   sleep_wake_entry на временном стеке: таблицы, регистры управления, MSR, FPU, APIC, TSC -> возврат в точку сна (cpu_ctx_save/jump) -> PIC, PIT,
   IOAPIC, часы, регистры ACPI -> повторный запуск остальных ядер (INIT-SIPI, тот же трамплин) -> \_BFS/\_WAK.
   Состояние ядра сохраняется целиком: GDT/IDT/TR, CR0/CR2/CR3/CR4, EFER, STAR/LSTAR/CSTAR/FMASK, FS/GS/KERNEL_GS, PAT, MTRR, FPU/SSE, регистры APIC. */
#include "../../platform.h"
#include "../../acpi.h"
#include "../../acpidev.h"
#include "../../uart.h"
#include "../../vmm.h"
#include "../../pci.h"
#include "../../heap.h"
#include "../../clock.h"
#include "../../rtc.h"
#include "../../timer.h"
#include "../../percpu.h"
#include "../../sched.h"
#include "acpirt.h"
#include "apic.h"
#include "ioapic.h"
#include "irqmode.h"
#include "idt.h"
#include "pic.h"
#include "smp.h"
#include "sleep_arch.h"
#include "cpu.h"

#define MSR_TSC        0x10
#define MSR_PAT        0x277
#define MSR_MTRR_CAP   0xFE
#define MSR_MTRR_DEF   0x2FF
#define MSR_EFER       0xC0000080
#define MSR_STAR       0xC0000081
#define MSR_LSTAR      0xC0000082
#define MSR_CSTAR      0xC0000083
#define MSR_FMASK      0xC0000084
#define MSR_FS_BASE    0xC0000100
#define MSR_GS_BASE    0xC0000101
#define MSR_KGS_BASE   0xC0000102
#define EFER_SCE       1ull
#define EFER_NXE       (1ull << 11)
#define MTRR_VAR_MAX   16
#define WAKE_STACK     8192
#define PM1_STS_ALL    0xFFFFu          /* запись единиц сбрасывает все состояния фиксированных событий PM1, включая WAK_STS (бит 15) */
#define PM1_WAK_STS    0x8000u
#define PM1_SLP_EN     (1u << 13)

static inline void outb_(uint16_t p, uint8_t v) { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outw_(uint16_t p, uint16_t v) { __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint16_t inw_(uint16_t p) { uint16_t v; __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint64_t rd_cr0(void) { uint64_t v; __asm__ volatile ("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint64_t rd_cr2(void) { uint64_t v; __asm__ volatile ("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint64_t rd_cr3(void) { uint64_t v; __asm__ volatile ("mov %%cr3, %0" : "=r"(v)); return v; }
static inline uint64_t rd_cr4(void) { uint64_t v; __asm__ volatile ("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void wr_cr0(uint64_t v) { __asm__ volatile ("mov %0, %%cr0" : : "r"(v) : "memory"); }
static inline void wr_cr2(uint64_t v) { __asm__ volatile ("mov %0, %%cr2" : : "r"(v) : "memory"); }
static inline void wr_cr3(uint64_t v) { __asm__ volatile ("mov %0, %%cr3" : : "r"(v) : "memory"); }
static inline void wr_cr4(uint64_t v) { __asm__ volatile ("mov %0, %%cr4" : : "r"(v) : "memory"); }

/* sleep_ctx.s: точка исполнения (callee-saved регистры, стек, адрес возврата) */
typedef struct { uint64_t r[8]; } ctx_asm_t;
extern uint64_t cpu_ctx_save(ctx_asm_t* c) __attribute__((returns_twice));
extern void cpu_ctx_jump(const ctx_asm_t* c) __attribute__((noreturn));

struct dtr { uint16_t limit; uint64_t base; } __attribute__((packed));

/* Всё состояние одного процессора на время сна. Первым - блок для sleep_ctx.s. */
typedef struct {
    ctx_asm_t a;
    uint64_t cr0, cr2, cr3, cr4, efer, star, lstar, cstar, fmask, fs_base, gs_base, kgs_base, pat, tsc;
    struct dtr gdtr, idtr;
    uint16_t tr;
    uint32_t mtrr_have, mtrr_vcnt;
    uint64_t mtrr_def, mtrr_fix[11], mtrr_var[2 * MTRR_VAR_MAX];
    apic_state_t lapic;
    uint8_t* wake_stack;                      /* временный стек пробуждения (у загрузочного ядра - общий статический) */
    volatile uint32_t parked, resumed;
    uint8_t fpu[512] __attribute__((aligned(16)));
} ctx_t;

static const uint32_t mtrr_fix_msr[11] = { 0x250, 0x258, 0x259, 0x268, 0x269, 0x26A, 0x26B, 0x26C, 0x26D, 0x26E, 0x26F };

static struct {
    ctx_t** ctx; unsigned n;                  /* по записи на каждое ядро таблицы percpu */
    volatile uint32_t park_req;               /* 0 - работаем, 1 - ядра встают и ждут, 2 - ядра останавливаются (hlt) */
    uint64_t irqf;
    char why[96];
} S;
static uint8_t bsp_wake_stack[16384] __attribute__((aligned(16)));

/* ---------------- состояние процессора ---------------- */

static void mtrr_capture(ctx_t* c) {
    uint32_t a, b, cc, d;
    c->mtrr_have = 0;
    cpuid(1, &a, &b, &cc, &d);
    if (!(d & (1u << 12))) return;                                  /* MTRR нет */
    uint64_t cap = cpu_rdmsr(MSR_MTRR_CAP);
    c->mtrr_vcnt = (uint32_t)(cap & 0xFF);
    if (c->mtrr_vcnt > MTRR_VAR_MAX) c->mtrr_vcnt = MTRR_VAR_MAX;
    c->mtrr_def = cpu_rdmsr(MSR_MTRR_DEF);
    if (cap & (1u << 8)) for (unsigned i = 0; i < 11; i++) c->mtrr_fix[i] = cpu_rdmsr(mtrr_fix_msr[i]);
    for (unsigned i = 0; i < c->mtrr_vcnt; i++) { c->mtrr_var[2 * i] = cpu_rdmsr(0x200 + 2 * i); c->mtrr_var[2 * i + 1] = cpu_rdmsr(0x201 + 2 * i); }
    c->mtrr_have = 1 | ((cap & (1u << 8)) ? 2u : 0u);
}

/* Порядок записи MTRR - как требует руководство: кэш выключен (CD), сброшен, MTRR отключены на время записи, затем включены. */
static void mtrr_restore(const ctx_t* c) {
    if (!c->mtrr_have) return;
    uint64_t cr0 = rd_cr0(), cr4 = rd_cr4();
    wr_cr0((cr0 | (1ull << 30)) & ~(1ull << 29));
    __asm__ volatile ("wbinvd" : : : "memory");
    wr_cr4(cr4 & ~(1ull << 7));
    wr_cr3(rd_cr3());
    cpu_wrmsr(MSR_MTRR_DEF, 0);
    if (c->mtrr_have & 2u) for (unsigned i = 0; i < 11; i++) cpu_wrmsr(mtrr_fix_msr[i], c->mtrr_fix[i]);
    for (unsigned i = 0; i < c->mtrr_vcnt; i++) { cpu_wrmsr(0x200 + 2 * i, c->mtrr_var[2 * i]); cpu_wrmsr(0x201 + 2 * i, c->mtrr_var[2 * i + 1]); }
    cpu_wrmsr(MSR_MTRR_DEF, c->mtrr_def);
    __asm__ volatile ("wbinvd" : : : "memory");
    wr_cr3(rd_cr3());
    wr_cr0(cr0);
    wr_cr4(cr4);
}

static void ctx_capture(ctx_t* c) {
    __asm__ volatile ("sgdt %0" : "=m"(c->gdtr));
    __asm__ volatile ("sidt %0" : "=m"(c->idtr));
    __asm__ volatile ("str %0" : "=r"(c->tr));
    c->cr0 = rd_cr0(); c->cr2 = rd_cr2(); c->cr3 = rd_cr3(); c->cr4 = rd_cr4();
    c->efer = cpu_rdmsr(MSR_EFER); c->star = cpu_rdmsr(MSR_STAR); c->lstar = cpu_rdmsr(MSR_LSTAR);
    c->cstar = cpu_rdmsr(MSR_CSTAR); c->fmask = cpu_rdmsr(MSR_FMASK);
    c->fs_base = cpu_rdmsr(MSR_FS_BASE); c->gs_base = cpu_rdmsr(MSR_GS_BASE); c->kgs_base = cpu_rdmsr(MSR_KGS_BASE);
    c->pat = cpu_rdmsr(MSR_PAT);
    mtrr_capture(c);
    fpu_save(c->fpu);
    apic_save(&c->lapic);
    c->tsc = rdtsc();
}

/* Возврат состояния (кроме CR3 и точки исполнения): прерывания выключены, мы на временном стеке с таблицами страниц ядра. */
static void ctx_restore(const ctx_t* c) {
    struct dtr g = c->gdtr, i = c->idtr;
    if (c->tr) {                                                       /* ltr помечает дескриптор занятым: в снимке он занят, возвращаем «доступен» */
        uint8_t* acc = (uint8_t*)(uintptr_t)g.base + (c->tr & ~7u) + 5;
        if ((*acc & 0x0F) == 0x0B) *acc = (uint8_t)(*acc & ~0x02);
    }
    __asm__ volatile (
        "lgdt %0\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"
        "1:\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        : : "m"(g) : "rax", "memory");
    if (c->tr) __asm__ volatile ("ltr %w0" : : "r"(c->tr));
    __asm__ volatile ("lidt %0" : : "m"(i));
    wr_cr4(c->cr4);
    uint64_t e = cpu_rdmsr(MSR_EFER), want = (e & ~(EFER_SCE | EFER_NXE)) | (c->efer & (EFER_SCE | EFER_NXE));   /* LME/LMA трогать нельзя */
    if (e != want) cpu_wrmsr(MSR_EFER, want);
    mtrr_restore(c);
    cpu_wrmsr(MSR_PAT, c->pat);
    cpu_wrmsr(MSR_STAR, c->star); cpu_wrmsr(MSR_LSTAR, c->lstar); cpu_wrmsr(MSR_CSTAR, c->cstar); cpu_wrmsr(MSR_FMASK, c->fmask);
    wr_cr0(c->cr0);
    wr_cr2(c->cr2);
    fpu_restore(c->fpu);
    cpu_wrmsr(MSR_FS_BASE, c->fs_base);                              /* базы - после загрузки селекторов: загрузка в FS/GS их обнуляет */
    cpu_wrmsr(MSR_GS_BASE, c->gs_base);
    cpu_wrmsr(MSR_KGS_BASE, c->kgs_base);
}

/* ---------------- точка пробуждения ядра ---------------- */

/* Сюда ядро попадает из трамплина (длинный режим, временный стек, таблицы страниц ядра, прерывания выключены): загрузочное - из вектора
   пробуждения прошивки, остальные - после INIT-SIPI. Состояние возвращается и исполнение продолжается в точке сна. */
void sleep_wake_entry(uint64_t idx) {
    ctx_t* c = S.ctx[idx];
    ctx_restore(c);
    (void)apic_restore(&c->lapic);
    if (idx == 0) cpu_wrmsr(MSR_TSC, c->tsc);                        /* часы ядра не должны прыгать: счёт продолжается с момента сна */
    else smp_resume_sync_ap();                                        /* остальные сверяются с загрузочным */
    wr_cr3(c->cr3);
    cpu_ctx_jump(&c->a);
}

/* ---------------- остальные ядра ---------------- */

int sleep_park_pending(void) { return __atomic_load_n(&S.park_req, __ATOMIC_ACQUIRE) != 0; }

/* Ядро (не загрузочное) встаёт. Прерывания выключены; ядро исполняет свой холостой поток (замороженный планировщик гарантирует, что чужих
   потоков на нём нет). Состояние сохраняется до объявления «встало»: загрузочное ядро может усыплять машину сразу после. */
void sleep_ap_park(void) {
    unsigned id = this_cpu()->cpu_id;
    if (id >= S.n || !S.ctx || !S.ctx[id]) return;
    ctx_t* c = S.ctx[id];
    ctx_capture(c);
    if (cpu_ctx_save(&c->a) == 0) {
        __atomic_store_n(&c->parked, 1, __ATOMIC_RELEASE);
        for (;;) {
            uint32_t r = __atomic_load_n(&S.park_req, __ATOMIC_ACQUIRE);
            if (r == 0) { __atomic_store_n(&c->parked, 0, __ATOMIC_RELEASE); return; }       /* отмена: сон не состоялся, ядро и не останавливалось */
            if (r == 2) for (;;) __asm__ volatile ("cli; hlt");                              /* сон: дальше только INIT-SIPI */
            __asm__ volatile ("pause");
        }
    }
    __atomic_store_n(&c->parked, 0, __ATOMIC_RELEASE);                 /* проснулись: состояние уже возвращено sleep_wake_entry */
    __atomic_store_n(&c->resumed, 1, __ATOMIC_RELEASE);
}

static int cpu_online_ap(unsigned i) { const percpu_t* p = &percpu_table[i]; return i != 0 && p->online && !p->failed; }

static void ctx_free_all(void) {
    if (!S.ctx) return;
    for (unsigned i = 0; i < S.n; i++) if (S.ctx[i]) { if (S.ctx[i]->wake_stack) kfree(S.ctx[i]->wake_stack); kfree(S.ctx[i]); }
    kfree(S.ctx); S.ctx = 0; S.n = 0;
}

static int ctx_alloc_all(void) {
    S.n = percpu_count();
    S.ctx = (ctx_t**)kmalloc(S.n * sizeof(ctx_t*));
    if (!S.ctx) return -1;
    for (unsigned i = 0; i < S.n; i++) S.ctx[i] = 0;
    for (unsigned i = 0; i < S.n; i++) {
        if (i != 0 && !cpu_online_ap(i)) continue;
        ctx_t* c = (ctx_t*)kmalloc(sizeof(ctx_t));
        if (!c) { ctx_free_all(); return -1; }
        uint8_t* z = (uint8_t*)c; for (unsigned k = 0; k < sizeof(*c); k++) z[k] = 0;
        S.ctx[i] = c;
        if (i != 0) {
            c->wake_stack = (uint8_t*)kmalloc(WAKE_STACK);
            if (!c->wake_stack) { ctx_free_all(); return -1; }
        }
    }
    return 0;
}

/* Загрузочное ядро просит остальные встать; ждёт, пока каждое сохранит состояние. 0 - все встали. */
static int park_others(void) {
    __atomic_store_n(&S.park_req, 1, __ATOMIC_SEQ_CST);
    for (unsigned i = 1; i < S.n; i++) if (cpu_online_ap(i)) (void)apic_send_ipi(percpu_table[i].apic_id, ICR_FIXED(IPI_RESCHED_VECTOR));
    uint64_t end = clock_ns() + 2000000000ull;
    for (;;) {
        unsigned left = 0;
        for (unsigned i = 1; i < S.n; i++) if (cpu_online_ap(i) && !__atomic_load_n(&S.ctx[i]->parked, __ATOMIC_ACQUIRE)) left++;
        if (!left) return 0;
        if (clock_ns() > end) {
            uart_print("[KIRA SLEEP] core did not stop within 2 s: "); uart_print_dec(left); uart_print(" core(s) still busy, sleep cancelled.\n");
            __atomic_store_n(&S.park_req, 0, __ATOMIC_SEQ_CST);
            return -1;
        }
        __asm__ volatile ("pause");
    }
}

/* ---------------- ACPI ---------------- */

static volatile uint8_t* facs_ptr(const acpi_info_t* a) {
    uint64_t pa = a->facs_phys;
    if (!pa) return 0;
    if (pa + 64 <= (1ull << 30)) return (volatile uint8_t*)(uintptr_t)pa;                 /* первый гигабайт - тождественная карта ядра */
    uint64_t page = pa & ~0xFFFull;
    volatile uint8_t* base = (volatile uint8_t*)vmm_map_mmio(page, 0x2000, VMM_MMIO_UC);
    return base ? base + (pa - page) : 0;
}

/* Вектор пробуждения в FACS: 32-битный реальный режим = адрес трамплина; 64-битный - 0 (прошивка берёт 32-битный). 0 - записан и проверен. */
static int facs_set_wake(const acpi_info_t* a, uint32_t vec, int verify_clear) {
    volatile uint8_t* f = facs_ptr(a);
    if (!f) return -1;
    uint8_t copy[64];
    for (unsigned i = 0; i < sizeof copy; i++) copy[i] = f[i];
    acpi_facs_t fc;
    if (acpi_facs_parse(copy, sizeof copy, &fc) != 0) return -2;
    *(volatile uint32_t*)(f + ACPI_FACS_WAKE32_OFF) = vec;
    if (fc.length >= 32) *(volatile uint64_t*)(f + ACPI_FACS_WAKE64_OFF) = 0;
    __asm__ volatile ("" ::: "memory");
    if (*(volatile uint32_t*)(f + ACPI_FACS_WAKE32_OFF) != vec) return -3;
    (void)verify_clear;
    return 0;
}

static int ensure_acpi_mode(const acpi_info_t* a) {
    uint16_t pm1 = (uint16_t)a->pm1a_cnt;
    if (inw_(pm1) & 1) return 0;
    if (a->smi_cmd && a->acpi_enable) {
        outb_((uint16_t)a->smi_cmd, a->acpi_enable);
        for (int i = 0; i < 3000000 && !(inw_(pm1) & 1); i++) cpu_relax();
    }
    return (inw_(pm1) & 1) ? 0 : -1;
}

/* Регистры питания чипсета. Блок PM1 (адрес PMBASE и разрешение ACPI) живёт в конфигурации моста LPC (Intel ICH/PCH, q35) или функции питания PIIX4
   (плата pc). Настоящая прошивка возвращает их на пути пробуждения до передачи управления системе; при сбросе платформы эмулятором они обнуляются, и порты PM1
   пропадают (чтение даёт 0xFFFF - его легко принять за "всё включено"). Поэтому: находится устройство, чей регистр 0x40 равен адресу PM1a_EVT, его конфигурация
   0x40..0xFC запоминается и, если после пробуждения регистр 0x40 не тот, возвращается (разрешающие регистры - последними). Сначала, до любого обращения к PM1. */
#define PMCFG_DW 48
static struct { int have; pci_dev_t d; uint32_t v[PMCFG_DW]; } pmcfg;
static void pmcfg_save(const acpi_info_t* a) {
    pmcfg.have = 0;
    if (!a || !a->pm1a_evt) return;
    for (int i = 0; i < pci_count(); i++) {
        const pci_dev_t* d = pci_get(i);
        if (!d || d->header_type & 0x7F) continue;
        if (d->class_code != 0x06 || (d->subclass != 0x01 && d->subclass != 0x80)) continue;
        if ((pci_read32(d, 0x40) & 0xFF80u & ~1u) != (a->pm1a_evt & 0xFF80u)) continue;
        pmcfg.d = *d;
        for (int k = 0; k < PMCFG_DW; k++) pmcfg.v[k] = pci_read32(d, (uint16_t)(0x40 + 4 * k));
        pmcfg.have = 1;
        return;
    }
}
static int pmcfg_restore(void) {
    if (!pmcfg.have) return 0;
    const pci_dev_t* d = &pmcfg.d;
    if (pci_read32(d, 0x40) == pmcfg.v[0]) return 0;                   /* прошивка вернула сама */
    pci_write32(d, 0x40, pmcfg.v[0]);
    for (int k = 1; k < PMCFG_DW; k++) {
        uint16_t off = (uint16_t)(0x40 + 4 * k);
        if (off == 0x44 || off == 0x80) continue;                      /* ICH: ACPI_CNTL; PIIX4: PMREGMISC - включают блок, ниже */
        pci_write32(d, off, pmcfg.v[k]);
    }
    pci_write32(d, 0x44, pmcfg.v[(0x44 - 0x40) / 4]);
    pci_write32(d, 0x80, pmcfg.v[(0x80 - 0x40) / 4]);
    return 1;
}

static void pm1_clear_status(const acpi_info_t* a) {
    if (a->pm1a_evt && a->pm1a_evt <= 0xFFFF) outw_((uint16_t)a->pm1a_evt, PM1_STS_ALL);
    if (a->pm1b_evt && a->pm1b_evt <= 0xFFFF) outw_((uint16_t)a->pm1b_evt, PM1_STS_ALL);
}

static int s3_values(const acpi_info_t* a, uint8_t* ta, uint8_t* tb) {
    if (acpirt_sx(3, ta, tb) == 0) return 0;                          /* вычисленный интерпретатором пакет \_S3 */
    if (a && a->s3_valid) { *ta = a->slp_typa_s3; *tb = a->slp_typb_s3; return 0; }
    return -1;
}

int platform_sleep_supported(const char** why) {
    const acpi_info_t* a = acpi_get();
    uint8_t ta, tb;
    const char* w = 0;
    if (!a || !a->valid) w = "no ACPI tables";
    else if (!a->pm1a_cnt || a->pm1a_cnt > 0xFFFF) w = "no PM1a control register in FADT";
    else if (s3_values(a, &ta, &tb) != 0) w = "firmware does not offer S3 (no \\_S3 package in DSDT)";
    else if (!a->facs_phys) w = "no FACS table (no wake vector)";
    else if (!apic_is_enabled()) w = "no Local APIC (cores cannot be restarted after sleep)";
    if (why) *why = w;
    return w == 0;
}

/* ---------------- сон ---------------- */

static void say_state(const char* s) { uart_print("[KIRA SLEEP] "); uart_print(s); uart_print("\n"); }

int platform_sleep(void) {
    const acpi_info_t* a = acpi_get();
    const char* why = 0;
    if (!platform_sleep_supported(&why)) { uart_print("[KIRA SLEEP] S3 unavailable: "); uart_print(why); uart_print(".\n"); return -1; }
    if (this_cpu()->cpu_id != 0) { say_state("internal: sleep must run on the boot core."); return -2; }
    uint8_t ta = 0, tb = 0;
    (void)s3_values(a, &ta, &tb);
    if (ensure_acpi_mode(a) != 0) { say_state("firmware did not enter ACPI mode (SCI_EN stays 0): sleep cancelled."); return -3; }
    if (ctx_alloc_all() != 0) { say_state("no memory for the processor state: sleep cancelled."); return -4; }
    if (ioapic_save() != 0 && irqmode_is_apic()) { say_state("no memory for the IOAPIC snapshot: sleep cancelled."); ctx_free_all(); return -4; }
    pic_suspend();

    acpirt_prepare_sleep(3);                                          /* \_PTS(3), \_GTS(3) - прерывания и потоки ещё живы */
    unsigned armed = 0;
    (void)acpirt_arm_wake(3, &armed);
    pmcfg_save(a);
    pm1_clear_status(a);
    uart_print("[KIRA SLEEP] entering S3: SLP_TYP "); uart_print_dec(ta); uart_print("/"); uart_print_dec(tb);
    uart_print(", wake sources armed: "); uart_print_dec(armed); uart_print(", cores to stop: "); uart_print_dec(smp_online() - 1); uart_print("\n");

    if (park_others() != 0) { ioapic_restore(); ctx_free_all(); return -5; }
    if (smp_tramp_install((uint64_t)(uintptr_t)bsp_wake_stack + sizeof bsp_wake_stack, sleep_wake_entry, 0) != 0 ||
        facs_set_wake(a, SMP_TRAMP_BASE, 0) != 0) {
        say_state("could not install the wake vector (trampoline or FACS): sleep cancelled.");
        __atomic_store_n(&S.park_req, 0, __ATOMIC_SEQ_CST);
        ioapic_restore(); ctx_free_all();
        return -6;
    }
    uart_flush_all();
    S.irqf = irq_save();
    ctx_t* c = S.ctx[0];
    ctx_capture(c);
    int slept;
    if (cpu_ctx_save(&c->a) == 0) {
        __atomic_store_n(&S.park_req, 2, __ATOMIC_SEQ_CST);           /* остальные ядра: hlt */
        c->tsc = rdtsc();                                              /* как можно позже: с этого значения часы продолжатся */
        __asm__ volatile ("wbinvd" : : : "memory");                    /* ACPI: кэши сбрасываются перед S3 */
        uint16_t pm1a = (uint16_t)a->pm1a_cnt;
        uint16_t va = acpi_pm1_sleep_value(inw_(pm1a), ta);
        outw_(pm1a, (uint16_t)(va & ~PM1_SLP_EN));                     /* сначала тип сна, затем включение (как делают остальные ОС) */
        uint16_t vb = 0;
        if (a->pm1b_cnt && a->pm1b_cnt <= 0xFFFF) { vb = acpi_pm1_sleep_value(inw_((uint16_t)a->pm1b_cnt), tb); outw_((uint16_t)a->pm1b_cnt, (uint16_t)(vb & ~PM1_SLP_EN)); }
        outw_(pm1a, va);
        if (a->pm1b_cnt && a->pm1b_cnt <= 0xFFFF) outw_((uint16_t)a->pm1b_cnt, vb);
        /* Настоящий S3 здесь не возвращается: процессор теряет питание или прошивка уводит его на вектор пробуждения. Если мы ещё здесь через 3 с
           (статус пробуждения не появился), машина сон не приняла. */
        uint64_t end = rdtsc() + clock_tsc_khz() * 3000;
        while (rdtsc() < end && !(a->pm1a_evt && (inw_((uint16_t)a->pm1a_evt) & PM1_WAK_STS))) cpu_relax();
        slept = 0;
    } else {
        slept = 1;
    }
    /* Сюда - либо из sleep_wake_entry (проснулись, состояние ядра уже возвращено), либо сразу, если машина не заснула. Локальные переменные,
       заданные до cpu_ctx_save, после пробуждения недостоверны: всё нужное берётся заново. */
    a = acpi_get();
    c = S.ctx[0];
    if (slept) {
        uart_init();                                                   /* COM1 после сброса: делитель, FIFO */
        pic_resume();
        timer_resume();
        (void)rtc_init();                                              /* часы реального времени: CMOS шла всё время сна */
    }
    (void)ioapic_restore();                                            /* линии устройств: как были (при несостоявшемся сне - те же значения, снимок освобождается) */
    if (slept && pmcfg_restore()) uart_print("[KIRA SLEEP] chipset: PM1 base and ACPI enable restored (the platform reset cleared them).\n");
    (void)facs_set_wake(a, 0, 1);
    (void)ensure_acpi_mode(a);
    pm1_clear_status(a);                                               /* WAK_STS и событие, разбудившее нас (кнопка): это пробуждение, не новое нажатие */
    acpirt_resume();                                                   /* режим контроллера прерываний (_PIC), события, линия SCI */
    __atomic_store_n(&S.park_req, 0, __ATOMIC_SEQ_CST);                /* до запуска ядер: проснувшееся не должно встать снова */
    unsigned back = 0, lost = 0;
    for (unsigned i = 1; i < S.n; i++) {
        if (!cpu_online_ap(i)) continue;
        ctx_t* ci = S.ctx[i];
        if (!__atomic_load_n(&ci->parked, __ATOMIC_ACQUIRE) && !slept) continue;
        ci->resumed = 0;
        /* Ядро, не отозвавшееся на INIT-SIPI, запускается заново (как делают остальные ОС: сообщение запуска может потеряться,
           пока ядро выходит из глубокого простоя; на медленной эмуляции окно запуска длиннее). Число попыток записывается в
           журнал, чтобы редкие повторы были видны, а не скрыты. */
        int ok = 0, attempt;
        for (attempt = 1; attempt <= 3 && !ok; attempt++) {
            ci->resumed = 0;
            ok = smp_tramp_install((uint64_t)(uintptr_t)ci->wake_stack + WAKE_STACK, sleep_wake_entry, i) == 0 && smp_ap_wake(i) == 0;
            if (ok) { uint64_t end = clock_ns() + 3000000000ull; while (!__atomic_load_n(&ci->resumed, __ATOMIC_ACQUIRE) && clock_ns() < end) cpu_relax(); ok = ci->resumed; }
        }
        attempt--;
        if (ok && attempt > 1) { uart_print("[KIRA SLEEP] core "); uart_print_dec(i); uart_print(" answered only on attempt "); uart_print_dec((uint64_t)attempt); uart_print(" of 3.\n"); }
        if (ok) back++;
        else {
            lost++;
            percpu_t* p = &percpu_table[i];
            __atomic_store_n(&p->failed, 1, __ATOMIC_RELEASE);
            uart_print("[KIRA SLEEP] core "); uart_print_dec(i); uart_print(" did not come back after sleep: left out of service.\n");
            smp_timekeeper_failed(i);
        }
    }
    acpirt_wake(3);                                                    /* \_BFS, \_WAK */
    ctx_free_all();
    uint64_t f = S.irqf;
    if (slept) {
        uart_print("[KIRA SLEEP] resumed from S3: cores back "); uart_print_dec(back); uart_print(", lost "); uart_print_dec(lost);
        uart_print(", clock continues (TSC "); uart_print_dec(rdtsc() / 1000000); uart_print(" M).\n");
    } else {
        say_state("machine did not enter S3 (no wake status within 3 s): state restored.");
    }
    irq_restore(f);
    return slept ? 1 : 0;
}
