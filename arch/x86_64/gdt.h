#ifndef GDT_H
#define GDT_H

#include <stdint.h>

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

// 16-байтный системный дескриптор TSS в long mode
struct tss_descriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];       // стеки для переходов в Ring 0/1/2
    uint64_t reserved1;
    uint64_t ist[7];       // IST1..IST7
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

// Селекторы
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   0x18
#define GDT_USER_CODE   0x20
#define GDT_TSS         0x28

// Номера IST (индексы 1..7 в поле ist дескриптора шлюза IDT)
#define IST_DOUBLE_FAULT 1
#define IST_NMI          2
#define IST_MACHINE_CHECK 3

void gdt_init(void);
void* gdt_init_ap(void);         /* свой GDT, TSS и IST-стеки для не загрузочного процессора; возвращает его TSS или 0 (нужна работающая куча) */
void tss_set_rsp0(uint64_t rsp0);

#endif
