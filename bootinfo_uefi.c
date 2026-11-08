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
    uint8_t *out = (uint8_t *)output;
    uint32_t mmap_size, fb_size = 0, total, off;
    int have_fb;

    if (!blob || !output || !output_size ||
        kbi_validate(blob, available) != 0 ||
        h->arch != KBI_ARCH_X86_64 || h->boot_mode != KBI_BOOT_UEFI)
        return -1;
    if (!h->mmap_count || h->mmap_count > KIRA_BOOTINFO_MAX_MMAP)
        return -2;

    map = kbi_mmap(blob);
    if (!map) return -2;

    if (h->mmap_count > (UINT32_MAX - MB2_MMAP_HEADER_SIZE) /
                        sizeof(mb2_mmap_entry_t))
        return -3;
    mmap_size = MB2_MMAP_HEADER_SIZE +
                h->mmap_count * (uint32_t)sizeof(mb2_mmap_entry_t);
    have_fb = !!(h->flags & KBI_F_FRAMEBUFFER);
    if (have_fb) fb_size = MB2_FB_RGB_SIZE;

    total = MB2_HEADER_SIZE + align8(mmap_size) +
            (have_fb ? align8(fb_size) : 0) + MB2_HEADER_SIZE;
    if (total > capacity || total > 65536u) return -4;

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

    *(uint32_t *)(out + off + 0) = MB2_TAG_END;
    *(uint32_t *)(out + off + 4) = MB2_HEADER_SIZE;
    *output_size = total;
    return 0;
}
