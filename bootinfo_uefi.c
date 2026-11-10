#include "bootinfo.h"
#include <stdint.h>

#define MB2_TAG_END 0u
#define MB2_TAG_MMAP 6u
#define MB2_TAG_FRAMEBUFFER 8u
#define MB2_HEADER_SIZE 8u
#define MB2_MMAP_HEADER_SIZE 16u
#define MB2_FB_RGB_SIZE 38u

typedef struct __attribute__((packed)) {
    uint32_t type, size;
    uint64_t addr;
    uint32_t pitch, width, height;
    uint8_t bpp, fb_type;
    uint16_t reserved;
    uint8_t red_pos, red_size, green_pos, green_size, blue_pos, blue_size;
} mb2_fb_tag_t;

typedef struct __attribute__((packed)) {
    uint64_t addr, length;
    uint32_t type, reserved;
} mb2_mmap_entry_t;

static uint32_t align8(uint32_t n) { return (n + 7u) & ~7u; }

static uint32_t mb2_memory_type(uint32_t type) {
    switch (type) {
    case KBI_MEM_USABLE: return 1;
    case KBI_MEM_ACPI_RECLAIM: return 3;
    case KBI_MEM_ACPI_NVS: return 4;
    case KBI_MEM_BAD: return 5;
    default: return 2;
    }
}

static void zero_bytes(void *dst, uint32_t count) {
    uint8_t *p = (uint8_t *)dst;
    for (uint32_t i = 0; i < count; ++i) p[i] = 0;
}

int kbi_to_multiboot2(const void *blob, uint32_t available,
                      void *output, uint32_t capacity, uint32_t *output_size) {
    const kbi_header_t *h = (const kbi_header_t *)blob;
    const kbi_mmap_entry_t *map;
    const kbi_module_t *mods;
    const uint8_t *source = (const uint8_t *)blob;
    uint8_t *out = (uint8_t *)output;
    uint32_t mmap_size, fb_size = 0, cmd_size = 0, total, off;
    uint64_t required;
    int have_fb;

    if (!blob || !output || !output_size ||
        kbi_validate(blob, available) != 0 ||
        h->arch != KBI_ARCH_X86_64 || h->boot_mode != KBI_BOOT_UEFI)
        return -1;
    if (!h->mmap_count || h->mmap_count > KIRA_BOOTINFO_MAX_MMAP)
        return -2;

    map = kbi_mmap(blob);
    mods = kbi_modules(blob);
    if (!map || (h->modules_count && !mods)) return -2;

    if (h->mmap_count > (UINT32_MAX - MB2_MMAP_HEADER_SIZE) /
                        sizeof(mb2_mmap_entry_t))
        return -3;
    mmap_size = MB2_MMAP_HEADER_SIZE +
                h->mmap_count * (uint32_t)sizeof(mb2_mmap_entry_t);
    have_fb = !!(h->flags & KBI_F_FRAMEBUFFER);
    if (have_fb) fb_size = MB2_FB_RGB_SIZE;

    required = MB2_HEADER_SIZE + align8(mmap_size) +
               (have_fb ? align8(fb_size) : 0) + MB2_HEADER_SIZE;
    if (h->cmdline_len) {
        if (h->cmdline_len > UINT32_MAX - MB2_HEADER_SIZE) return -3;
        cmd_size = MB2_HEADER_SIZE + h->cmdline_len;
        required += align8(cmd_size);
    }
    for (uint32_t i = 0; i < h->modules_count; ++i) {
        uint64_t end = mods[i].base + mods[i].length;
        uint64_t tag_size = 16u + (uint64_t)mods[i].name_len;
        if (mods[i].base > UINT32_MAX || end > UINT32_MAX ||
            tag_size > UINT32_MAX) return -5;
        required += align8((uint32_t)tag_size);
    }
    if (required > capacity || required > 65536u || required > UINT32_MAX)
        return -4;
    total = (uint32_t)required;

    zero_bytes(out, total);
    *(uint32_t *)(out + 0) = total;
    *(uint32_t *)(out + 4) = 0;

    off = MB2_HEADER_SIZE;
    *(uint32_t *)(out + off + 0) = MB2_TAG_MMAP;
    *(uint32_t *)(out + off + 4) = mmap_size;
    *(uint32_t *)(out + off + 8) = sizeof(mb2_mmap_entry_t);
    *(uint32_t *)(out + off + 12) = 0;
    mb2_mmap_entry_t *entries =
        (mb2_mmap_entry_t *)(out + off + MB2_MMAP_HEADER_SIZE);
    for (uint32_t i = 0; i < h->mmap_count; ++i) {
        entries[i].addr = map[i].base;
        entries[i].length = map[i].length;
        entries[i].type = mb2_memory_type(map[i].type);
        entries[i].reserved = 0;
    }
    off += align8(mmap_size);

    if (have_fb) {
        mb2_fb_tag_t *fb = (mb2_fb_tag_t *)(out + off);
        fb->type = MB2_TAG_FRAMEBUFFER;
        fb->size = fb_size;
        fb->addr = h->fb_phys;
        fb->pitch = h->fb_pitch;
        fb->width = h->fb_width;
        fb->height = h->fb_height;
        fb->bpp = h->fb_bpp;
        fb->fb_type = 1;
        fb->red_pos = h->fb_red_pos;
        fb->red_size = h->fb_red_size;
        fb->green_pos = h->fb_green_pos;
        fb->green_size = h->fb_green_size;
        fb->blue_pos = h->fb_blue_pos;
        fb->blue_size = h->fb_blue_size;
        off += align8(fb_size);
    }

    if (cmd_size) {
        *(uint32_t *)(out + off + 0) = 1u; /* Multiboot2 command-line tag */
        *(uint32_t *)(out + off + 4) = cmd_size;
        for (uint32_t i = 0; i < h->cmdline_len; ++i)
            out[off + MB2_HEADER_SIZE + i] = source[h->cmdline_off + i];
        off += align8(cmd_size);
    }

    for (uint32_t i = 0; i < h->modules_count; ++i) {
        uint32_t tag_size = 16u + mods[i].name_len;
        *(uint32_t *)(out + off + 0) = 3u; /* Multiboot2 module tag */
        *(uint32_t *)(out + off + 4) = tag_size;
        *(uint32_t *)(out + off + 8) = (uint32_t)mods[i].base;
        *(uint32_t *)(out + off + 12) = (uint32_t)(mods[i].base + mods[i].length);
        for (uint32_t j = 0; j < mods[i].name_len; ++j)
            out[off + 16u + j] = source[mods[i].name_off + j];
        off += align8(tag_size);
    }

    *(uint32_t *)(out + off + 0) = MB2_TAG_END;
    *(uint32_t *)(out + off + 4) = MB2_HEADER_SIZE;
    *output_size = total;
    return 0;
}
