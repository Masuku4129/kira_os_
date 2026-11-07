#ifndef KIRA_BOOTINFO_HANDOFF_H
#define KIRA_BOOTINFO_HANDOFF_H
#include <stdint.h>

#define KIRA_BOOTINFO_MAGIC 0x4b495241u /* KIRA */
#define KIRA_BOOTINFO_VERSION 1u
#define KIRA_BOOTINFO_MAX_MMAP 128u
#define KIRA_BOOTINFO_MAX_MODULES 16u

enum kira_mem_type {
    KIRA_MEM_USABLE = 1,
    KIRA_MEM_RESERVED = 2,
    KIRA_MEM_ACPI_RECLAIM = 3,
    KIRA_MEM_ACPI_NVS = 4,
    KIRA_MEM_BAD = 5,
    KIRA_MEM_BOOTLOADER = 6,
    KIRA_MEM_KERNEL = 7,
    KIRA_MEM_FRAMEBUFFER = 8
};

typedef struct {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t attributes;
} kira_memory_range_t;

typedef struct {
    uint64_t address;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} kira_boot_module_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint64_t flags;
    uint64_t rsdp;
    uint64_t framebuffer;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_format;
    uint32_t memory_count;
    uint32_t module_count;
    kira_memory_range_t memory[KIRA_BOOTINFO_MAX_MMAP];
    kira_boot_module_t modules[KIRA_BOOTINFO_MAX_MODULES];
} kira_bootinfo_t;

static inline int kira_bootinfo_validate(const kira_bootinfo_t *b) {
    if (!b || b->magic != KIRA_BOOTINFO_MAGIC ||
        b->version != KIRA_BOOTINFO_VERSION || b->size != sizeof(*b))
        return -1;
    if (b->memory_count > KIRA_BOOTINFO_MAX_MMAP ||
        b->module_count > KIRA_BOOTINFO_MAX_MODULES)
        return -1;
    for (uint32_t i = 0; i < b->memory_count; ++i) {
        uint64_t end = b->memory[i].base + b->memory[i].length;
        if (!b->memory[i].length || end < b->memory[i].base)
            return -1;
    }
    for (uint32_t i = 0; i < b->module_count; ++i) {
        uint64_t end = b->modules[i].address + b->modules[i].length;
        if (!b->modules[i].length || end < b->modules[i].address)
            return -1;
    }
    return 0;
}
#endif
