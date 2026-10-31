#include "gdt.h"
#include "percpu.h"
#include "uart.h"
#include "heap.h"

#define IST_STACK_SIZE 16384

static struct {
    struct gdt_entry entries[5];
    struct tss_descriptor tss_desc;
} __attribute__((packed)) gdt;

static struct gdt_ptr gp;
static struct tss tss;

static uint8_t ist_stacks[3][IST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t rsp0_stack[IST_STACK_SIZE] __attribute__((aligned(16)));

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt.entries[num].base_low = (base & 0xFFFF);
    gdt.entries[num].base_middle = (base >> 16) & 0xFF;
    gdt.entries[num].base_high = (base >> 24) & 0xFF;
    gdt.entries[num].limit_low = (limit & 0xFFFF);
    gdt.entries[num].granularity = ((limit >> 16) & 0x0F);
    gdt.entries[num].granularity |= (gran & 0xF0);
    gdt.entries[num].access = access;
}

void tss_set_rsp0(uint64_t rsp0) {
    struct tss* t = (struct tss*)this_cpu()->tss;     // у каждого ядра свой TSS; 0 - общий TSS загрузочного
    (t ? t : &tss)->rsp[0] = rsp0;
}

void gdt_init(void) {
    // Нулевой дескриптор
    gdt_set_gate(0, 0, 0, 0, 0);
    // 0x08: код ядра (Ring 0, Long Mode)
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0x20);
    // 0x10: данные ядра (Ring 0)
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0x00);
    // 0x18: данные пользователя (Ring 3)
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xF2, 0x00);
    // 0x20: код пользователя (Ring 3, Long Mode). Порядок data -> code нужен для SYSRET.
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xFA, 0x20);

    // TSS: rsp0 для будущих переходов из Ring 3 и три IST-стека для аварийных
    // исключений (#DF, NMI, #MC): они исполняются на ГАРАНТИРОВАННО исправном стеке,
    // даже если RSP повреждён (иначе двойной сбой превращается в triple fault).
    for (unsigned i = 0; i < sizeof(tss); i++) ((uint8_t*)&tss)[i] = 0;
    tss.rsp[0] = (uint64_t)(uintptr_t)(rsp0_stack + IST_STACK_SIZE);
    for (int i = 0; i < 3; i++) {
        tss.ist[i] = (uint64_t)(uintptr_t)(ist_stacks[i] + IST_STACK_SIZE);
    }
    tss.iomap_base = sizeof(tss); // карты портов нет: любой in/out из Ring 3 даст #GP

    uint64_t base = (uint64_t)(uintptr_t)&tss;
    gdt.tss_desc.limit_low = sizeof(tss) - 1;
    gdt.tss_desc.base_low = base & 0xFFFF;
    gdt.tss_desc.base_middle = (base >> 16) & 0xFF;
    gdt.tss_desc.access = 0x89; // present, DPL0, 64-bit TSS (available)
    gdt.tss_desc.granularity = 0;
    gdt.tss_desc.base_high = (base >> 24) & 0xFF;
    gdt.tss_desc.base_upper = (uint32_t)(base >> 32);
    gdt.tss_desc.reserved = 0;

    gp.limit = sizeof(gdt) - 1;
    gp.base = (uint64_t)(uintptr_t)&gdt;

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
        : : "m"(gp) : "rax", "memory"
    );

    __asm__ volatile ("ltr %w0" : : "r"((uint16_t)GDT_TSS));
    percpu_install(&percpu_table[0]);   /* загрузка селектора в %gs обнулила GS_BASE */

    uart_print("[KIRA GDT] GDT loaded (Ring0/Ring3 segments), TSS loaded with 3 IST stacks (#DF, NMI, #MC).\n");
}

// Не загрузочный процессор: копия GDT с собственным TSS (дескриптор TSS нельзя делить между процессорами:
// ltr помечает его занятым) и собственные IST-стеки для #DF, NMI, #MC.
void* gdt_init_ap(void) {
    uint8_t* raw = (uint8_t*)kmalloc(sizeof(gdt) + sizeof(struct tss) + 3 * IST_STACK_SIZE + IST_STACK_SIZE + 64);
    if (!raw) return 0;
    uintptr_t a = ((uintptr_t)raw + 15) & ~(uintptr_t)15;
    __typeof__(gdt)* g = (__typeof__(gdt)*)a;
    struct tss* t = (struct tss*)(a + sizeof(gdt));
    uint8_t* stacks = (uint8_t*)(((uintptr_t)(t + 1) + 15) & ~(uintptr_t)15);
    *g = gdt;
    for (unsigned i = 0; i < sizeof(*t); i++) ((uint8_t*)t)[i] = 0;
    t->rsp[0] = (uint64_t)(uintptr_t)(stacks + 3 * IST_STACK_SIZE + IST_STACK_SIZE);   // до первого потока процесса
    for (int i = 0; i < 3; i++) t->ist[i] = (uint64_t)(uintptr_t)(stacks + (i + 1) * IST_STACK_SIZE);
    t->iomap_base = sizeof(*t);
    uint64_t base = (uint64_t)(uintptr_t)t;
    g->tss_desc.limit_low = sizeof(*t) - 1;
    g->tss_desc.base_low = base & 0xFFFF;
    g->tss_desc.base_middle = (base >> 16) & 0xFF;
    g->tss_desc.access = 0x89;                      // доступный (не занятый) TSS
    g->tss_desc.granularity = 0;
    g->tss_desc.base_high = (base >> 24) & 0xFF;
    g->tss_desc.base_upper = (uint32_t)(base >> 32);
    g->tss_desc.reserved = 0;
    static struct gdt_ptr ap_gp;                    // lgdt читает его сразу; процессоры стартуют по одному
    ap_gp.limit = sizeof(*g) - 1;
    ap_gp.base = (uint64_t)(uintptr_t)g;
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
        : : "m"(ap_gp) : "rax", "memory"
    );
    __asm__ volatile ("ltr %w0" : : "r"((uint16_t)GDT_TSS));
    return t;
}
