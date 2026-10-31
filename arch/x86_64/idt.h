#ifndef IDT_H
#define IDT_H

#include <stdint.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define APIC_SPURIOUS_VECTOR 0xFE

void idt_init(void);
void idt_load_this_cpu(void);        /* IDTR для не загрузочного процессора (таблица общая) */
#define IPI_PING_VECTOR   0xF0
#define IPI_RESCHED_VECTOR 0xF1   /* пора выбрать поток заново (простаивающее ядро будят, не дожидаясь тика) */
#define IPI_TLB_VECTOR    0xF2    /* сбросить записи TLB адресного пространства (vmm_tlb_shootdown) */
void idt_set_gate(uint8_t vector, void* handler, uint16_t selector, uint8_t flags);
void load_idt(struct idt_ptr* ptr);

#endif
