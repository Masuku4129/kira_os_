#include "idt.h"
#include "uart.h"
#include "gdt.h"

__attribute__((aligned(0x10))) static struct idt_entry idt[256];
static struct idt_ptr idtp;

extern void* isr_stub_table[];   // 48 записей: исключения 0..31 и IRQ 32..47
extern void isr_unhandled(void); // вектор 255: "неожиданное прерывание"
extern void isr_ipi_ping(void);
extern void isr_ipi_resched(void);
extern void isr_ipi_tlb(void);
extern void isr_spurious(void);  // ложное прерывание LAPIC (вектор 0xFE): просто iretq

void idt_set_gate(uint8_t vector, void* handler, uint16_t selector, uint8_t flags) {
    uint64_t handler_addr = (uint64_t)handler;
    idt[vector].offset_low  = (uint16_t)(handler_addr & 0xFFFF);
    idt[vector].selector    = selector;
    idt[vector].ist         = 0;
    idt[vector].type_attr   = flags; // 0x8E = Present, Ring 0, Interrupt Gate
    idt[vector].offset_mid  = (uint16_t)((handler_addr >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)((handler_addr >> 32) & 0xFFFFFFFF);
    idt[vector].zero        = 0;
}

void idt_load_this_cpu(void) { load_idt(&idtp); }

void idt_init(void) {
    uart_print("[KIRA IDT] Configuring 64-bit Interrupt Descriptor Table...\n");

    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base  = (uint64_t)&idt[0];

    // Все векторы по умолчанию ведут в обработчик "неожиданного прерывания",
    // а не на адрес 0.
    for (int i = 0; i < 256; i++) {
        idt_set_gate((uint8_t)i, (void*)isr_unhandled, 0x08, 0x8E);
    }

    // Получаем таблицу указателей из ассемблера
    void** stubs = isr_stub_table;

    // Исключения CPU (0-31) и аппаратные IRQ0..IRQ15 (32-47)
    for (int i = 0; i < 48; i++) {
        idt_set_gate((uint8_t)i, stubs[i], 0x08, 0x8E);
    }

    // Аварийные исключения работают на отдельных IST-стеках (см. gdt.c)
    idt[8].ist  = IST_DOUBLE_FAULT;
    idt[2].ist  = IST_NMI;
    idt[18].ist = IST_MACHINE_CHECK;

    // Ложное прерывание Local APIC: обрабатывается без EOI
    idt_set_gate(APIC_SPURIOUS_VECTOR, (void*)isr_spurious, 0x08, 0x8E);

    idt_set_gate(IPI_PING_VECTOR, (void*)isr_ipi_ping, 0x08, 0x8E);   // межъядерный пинг (smp.c)
    idt_set_gate(IPI_RESCHED_VECTOR, (void*)isr_ipi_resched, 0x08, 0x8E);
    idt_set_gate(IPI_TLB_VECTOR, (void*)isr_ipi_tlb, 0x08, 0x8E);

    // Загружаем IDTR через надежную ассемблерную функцию
    load_idt(&idtp);

    uart_print("[KIRA IDT] IDT loaded: 32 exceptions (#DF/NMI/#MC on IST) + 16 IRQs + LAPIC spurious, other vectors -> unhandled trap.\n");
}
