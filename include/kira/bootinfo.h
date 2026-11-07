#ifndef KIRA_BOOTINFO_H
#define KIRA_BOOTINFO_H

#include <stdint.h>

#define KIRA_BOOTINFO_MAGIC 0x314942454152494bull
#define KIRA_BOOTINFO_VERSION 1u

enum kira_boot_source {
    KIRA_BOOT_BIOS = 1,
    KIRA_BOOT_UEFI = 2
};

enum kira_framebuffer_format {
    KIRA_FB_NONE = 0,
    KIRA_FB_RGBX8 = 1,
    KIRA_FB_BGRX8 = 2
};

struct kira_boot_memory_map {
    uint64_t address;
    uint64_t byte_size;
    uint64_t descriptor_size;
    uint32_t descriptor_version;
    uint32_t reserved;
};

struct kira_boot_framebuffer {
    uint64_t address;
    uint64_t byte_size;
    uint32_t width;
    uint32_t height;
    uint32_t pixels_per_scanline;
    uint32_t format;
};

struct kira_bootinfo {
    uint64_t magic;
    uint32_t version;
    uint32_t structure_size;
    uint32_t boot_source;
    uint32_t flags;
    struct kira_boot_memory_map memory;
    struct kira_boot_framebuffer framebuffer;
    uint64_t rsdp_address;
    uint64_t kernel_physical_base;
    uint64_t kernel_image_size;
    uint64_t initrd_physical_base;
    uint64_t initrd_size;
    uint64_t command_line_physical;
    uint32_t command_line_length;
    uint32_t reserved;
};

static inline int kira_bootinfo_valid(const struct kira_bootinfo *info,
                                      uint64_t available_size)
{
    return info &&
           available_size >= sizeof(*info) &&
           info->magic == KIRA_BOOTINFO_MAGIC &&
           info->version == KIRA_BOOTINFO_VERSION &&
           info->structure_size >= sizeof(*info) &&
           info->structure_size <= available_size;
}

#endif
