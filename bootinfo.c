#include "bootinfo.h"
#include <stddef.h>

#define KBI_ALLOWED_FLAGS (KBI_F_FRAMEBUFFER | KBI_F_ACPI | KBI_F_FDT | KBI_F_CMDLINE)

static int range_ok(uint32_t off, uint64_t count, uint64_t stride,
                    uint32_t total, uint32_t align) {
    if (!count) return off == 0;
    if (!off || (off & (align - 1)) || off < sizeof(kbi_header_t)) return 0;
    if (!stride || count > UINT64_MAX / stride) return 0;
    uint64_t bytes = count * stride;
    return bytes <= total && off <= total - bytes;
}

static int overlaps(uint64_t a, uint64_t an, uint64_t b, uint64_t bn) {
    if (!an || !bn) return 0;
    return a < b + bn && b < a + an;
}

static int bit_ranges_overlap(uint8_t ap, uint8_t an, uint8_t bp, uint8_t bn) {
    return (uint16_t)ap < (uint16_t)bp + bn &&
           (uint16_t)bp < (uint16_t)ap + an;
}

int kbi_validate(const void* blob, uint32_t available) {
    if (!blob || available < sizeof(kbi_header_t)) return -1;
    const kbi_header_t* h = (const kbi_header_t*)blob;
    if (h->magic != KIRA_BOOTINFO_MAGIC || h->version != KIRA_BOOTINFO_VERSION ||
        h->header_size != sizeof(*h) || h->total_size < sizeof(*h) ||
        h->total_size > KIRA_BOOTINFO_MAX_SIZE || h->total_size > available) return -2;
    if ((h->arch != KBI_ARCH_X86_64 && h->arch != KBI_ARCH_AARCH64) ||
        (h->boot_mode < KBI_BOOT_BIOS || h->boot_mode > KBI_BOOT_MULTIBOOT2) ||
        (h->boot_mode == KBI_BOOT_BIOS && h->arch != KBI_ARCH_X86_64) ||
        (h->boot_mode == KBI_BOOT_MULTIBOOT2 && h->arch != KBI_ARCH_X86_64) ||
        (h->boot_mode == KBI_BOOT_DEVICE_TREE && h->arch != KBI_ARCH_AARCH64)) return -3;
    if (h->reserved0 || h->reserved1 || h->fb_reserved ||
        (h->flags & ~KBI_ALLOWED_FLAGS)) return -4;

    if (!h->kernel_phys_base || !h->kernel_size ||
        h->kernel_size > UINT64_MAX - h->kernel_phys_base) return -10;

    if (h->mmap_count > KIRA_BOOTINFO_MAX_MMAP ||
        (h->mmap_count && h->mmap_entry_size != sizeof(kbi_mmap_entry_t)) ||
        (!h->mmap_count && h->mmap_entry_size != 0 &&
         h->mmap_entry_size != sizeof(kbi_mmap_entry_t)) ||
        !range_ok(h->mmap_off, h->mmap_count, h->mmap_entry_size,
                  h->total_size, 8)) return -5;
    if (h->modules_count > KIRA_BOOTINFO_MAX_MODULES ||
        (h->modules_count && h->module_entry_size != sizeof(kbi_module_t)) ||
        (!h->modules_count && h->module_entry_size != 0 &&
         h->module_entry_size != sizeof(kbi_module_t)) ||
        !range_ok(h->modules_off, h->modules_count, h->module_entry_size,
                  h->total_size, 8)) return -6;

    uint64_t mmap_bytes = (uint64_t)h->mmap_count * h->mmap_entry_size;
    uint64_t module_bytes = (uint64_t)h->modules_count * h->module_entry_size;
    if (overlaps(h->mmap_off, mmap_bytes, h->modules_off, module_bytes)) return -13;

    if (h->cmdline_len) {
        if (!h->cmdline_off || h->cmdline_off < sizeof(*h) ||
            h->cmdline_off > h->total_size ||
            h->cmdline_len > h->total_size - h->cmdline_off) return -7;
        if (overlaps(h->cmdline_off, h->cmdline_len, h->mmap_off, mmap_bytes) ||
            overlaps(h->cmdline_off, h->cmdline_len, h->modules_off, module_bytes)) return -13;
        const char* s = (const char*)blob + h->cmdline_off;
        if (s[h->cmdline_len - 1] != 0) return -7;
    } else if (h->cmdline_off) return -7;

    if (!!(h->flags & KBI_F_FRAMEBUFFER) != !!h->fb_size) return -8;
    if (h->fb_size) {
        uint64_t min_pitch = ((uint64_t)h->fb_width * h->fb_bpp + 7) / 8;
        if (!h->fb_phys || h->fb_size > UINT64_MAX - h->fb_phys ||
            overlaps(h->fb_phys, h->fb_size, h->kernel_phys_base, h->kernel_size) ||
            !h->fb_width || !h->fb_height || !h->fb_pitch ||
            (h->fb_bpp != 15 && h->fb_bpp != 16 &&
             h->fb_bpp != 24 && h->fb_bpp != 32) ||
            h->fb_pitch < min_pitch ||
            (uint64_t)h->fb_pitch * h->fb_height > h->fb_size ||
            !h->fb_red_size || !h->fb_green_size || !h->fb_blue_size ||
            (uint16_t)h->fb_red_pos + h->fb_red_size > h->fb_bpp ||
            (uint16_t)h->fb_green_pos + h->fb_green_size > h->fb_bpp ||
            (uint16_t)h->fb_blue_pos + h->fb_blue_size > h->fb_bpp ||
            bit_ranges_overlap(h->fb_red_pos, h->fb_red_size,
                               h->fb_green_pos, h->fb_green_size) ||
            bit_ranges_overlap(h->fb_red_pos, h->fb_red_size,
                               h->fb_blue_pos, h->fb_blue_size) ||
            bit_ranges_overlap(h->fb_green_pos, h->fb_green_size,
                               h->fb_blue_pos, h->fb_blue_size)) return -8;
    }
    if (!!(h->flags & KBI_F_ACPI) != !!h->rsdp_phys ||
        !!(h->flags & KBI_F_FDT) != !!h->fdt_phys ||
        !!(h->flags & KBI_F_CMDLINE) != !!h->cmdline_len ||
        (h->rsdp_phys && (h->rsdp_phys & 15)) ||
        (h->fdt_phys && (h->fdt_phys & 7))) return -9;

    const kbi_mmap_entry_t* mm =
        (const kbi_mmap_entry_t*)((const uint8_t*)blob + h->mmap_off);
    uint64_t previous_end = 0;
    for (uint32_t i = 0; i < h->mmap_count; i++) {
        if (!mm[i].length || mm[i].base > UINT64_MAX - mm[i].length ||
            mm[i].type < KBI_MEM_USABLE || mm[i].type > KBI_MEM_FRAMEBUFFER)
            return -11;
        if (i && mm[i].base < previous_end) return -11;
        previous_end = mm[i].base + mm[i].length;
    }

    const kbi_module_t* mods =
        (const kbi_module_t*)((const uint8_t*)blob + h->modules_off);
    for (uint32_t i = 0; i < h->modules_count; i++) {
        if (!mods[i].length || mods[i].base > UINT64_MAX - mods[i].length ||
            overlaps(mods[i].base, mods[i].length, h->kernel_phys_base,
                     h->kernel_size) ||
            (h->fb_size && overlaps(mods[i].base, mods[i].length, h->fb_phys, h->fb_size)) ||
            !mods[i].name_len || mods[i].reserved ||
            mods[i].name_off < sizeof(*h) ||
            mods[i].name_off > h->total_size ||
            mods[i].name_len > h->total_size - mods[i].name_off ||
            overlaps(mods[i].name_off, mods[i].name_len, h->mmap_off, mmap_bytes) ||
            overlaps(mods[i].name_off, mods[i].name_len, h->modules_off, module_bytes) ||
            overlaps(mods[i].name_off, mods[i].name_len, h->cmdline_off, h->cmdline_len))
            return -12;
        const char* n = (const char*)blob + mods[i].name_off;
        if (n[mods[i].name_len - 1] != 0) return -12;
        /* Embedded NULs make a module's byte name differ from its C-visible name. */
        for (uint32_t k = 0; k + 1 < mods[i].name_len; k++)
            if (!n[k]) return -12;
        for (uint32_t j = 0; j < i; j++) {
            const char* prev = (const char*)blob + mods[j].name_off;
            if (overlaps(mods[i].base, mods[i].length,
                         mods[j].base, mods[j].length) ||
                overlaps(mods[i].name_off, mods[i].name_len,
                         mods[j].name_off, mods[j].name_len)) return -12;
            uint32_t a = mods[i].name_len - 1, b = mods[j].name_len - 1;
            if (a == b) {
                uint32_t k = 0;
                while (k < a && n[k] == prev[k]) k++;
                if (k == a) return -12;
            }
        }
    }
    return 0;
}

/* Accessors require a blob already accepted by kbi_validate(). */
const kbi_mmap_entry_t* kbi_mmap(const void* blob) {
    const kbi_header_t* h = (const kbi_header_t*)blob;
    return h && h->mmap_count
        ? (const kbi_mmap_entry_t*)((const uint8_t*)blob + h->mmap_off) : 0;
}
const kbi_module_t* kbi_modules(const void* blob) {
    const kbi_header_t* h = (const kbi_header_t*)blob;
    return h && h->modules_count
        ? (const kbi_module_t*)((const uint8_t*)blob + h->modules_off) : 0;
}
