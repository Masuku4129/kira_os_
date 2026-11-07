#include "bootinfo.h"
#include <stddef.h>

static uint32_t uefi_mem_type(uint32_t t) {
    switch (t) {
    case 7: return KBI_MEM_USABLE;       /* EfiConventionalMemory */
    case 9: return KBI_MEM_ACPI_RECLAIM;
    case 10: return KBI_MEM_ACPI_NVS;
    case 8: return KBI_MEM_BAD;          /* EfiUnusableMemory */
    default: return KBI_MEM_RESERVED;   /* loader/runtime/MMIO remain reserved */
    }
}

int kbi_from_uefi_memory_map(const void* descriptors, uint32_t map_size,
                            uint32_t descriptor_size, uint64_t rsdp_phys,
                            uint64_t kernel_base, uint64_t kernel_size,
                            void* out, uint32_t capacity, uint32_t* out_size) {
    if (!descriptors || !out || !out_size ||
        descriptor_size < sizeof(kbi_uefi_memory_desc_t) ||
        map_size == 0 || map_size % descriptor_size ||
        !kernel_base || !kernel_size || kernel_base > UINT64_MAX - kernel_size ||
        (rsdp_phys && (rsdp_phys & 15u))) return -1;
    uint32_t n = map_size / descriptor_size;
    if (n > KIRA_BOOTINFO_MAX_MMAP) return -2;
    uint64_t max_entries = (uint64_t)n + 2u;
    uint64_t need = sizeof(kbi_header_t) + max_entries * sizeof(kbi_mmap_entry_t);
    if (need > capacity || need > KIRA_BOOTINFO_MAX_SIZE) return -3;

    uint8_t *blob = (uint8_t *)out;
    for (uint32_t i = 0; i < (uint32_t)need; i++) blob[i] = 0;
    kbi_header_t *h = (kbi_header_t *)blob;
    h->magic = KIRA_BOOTINFO_MAGIC;
    h->version = KIRA_BOOTINFO_VERSION;
    h->header_size = sizeof(*h);
    h->arch = KBI_ARCH_X86_64;
    h->boot_mode = KBI_BOOT_UEFI;
    h->mmap_off = (uint32_t)((sizeof(*h) + 7u) & ~7u);
    h->mmap_entry_size = sizeof(kbi_mmap_entry_t);
    h->kernel_phys_base = kernel_base;
    h->kernel_size = kernel_size;
    h->loader_id = 0x49464555u; /* "UEFI" */
    if (rsdp_phys) { h->rsdp_phys = rsdp_phys; h->flags |= KBI_F_ACPI; }

    kbi_mmap_entry_t *mm = (kbi_mmap_entry_t *)(blob + h->mmap_off);
    uint32_t count = 0;
    for (uint32_t i = 0; i < n; i++) {
        const kbi_uefi_memory_desc_t *d =
            (const kbi_uefi_memory_desc_t *)((const uint8_t *)descriptors +
                                               (size_t)i * descriptor_size);
        if (!d->number_of_pages) continue;
        if (d->number_of_pages > UINT64_MAX / 4096u ||
            d->physical_start > UINT64_MAX - d->number_of_pages * 4096u)
            return -4;
        uint64_t length = d->number_of_pages * 4096u;
        mm[count].base = d->physical_start;
        mm[count].length = length;
        mm[count].type = uefi_mem_type(d->type);
        count++;
    }
    for (uint32_t i = 1; i < count; i++) {
        kbi_mmap_entry_t v = mm[i];
        uint32_t j = i;
        while (j && mm[j - 1].base > v.base) { mm[j] = mm[j - 1]; j--; }
        mm[j] = v;
    }
    /* Reserve the kernel's physical range explicitly. UEFI loader pages
       must never re-enter the allocator as ordinary conventional memory. */
    uint64_t kernel_end = kernel_base + kernel_size;
    uint32_t owner = count;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t end = mm[i].base + mm[i].length;
        if (kernel_base >= mm[i].base && kernel_end <= end) { owner = i; break; }
    }
    if (owner == count || count + 2u > KIRA_BOOTINFO_MAX_MMAP) return -6;
    {
        kbi_mmap_entry_t original = mm[owner];
        uint64_t original_end = original.base + original.length;
        uint32_t extra = (kernel_base > original.base ? 1u : 0u) +
                         (kernel_end < original_end ? 1u : 0u);
        for (uint32_t i = count; i > owner + 1u; i--) mm[i + extra - 1u] = mm[i - 1u];
        uint32_t at = owner;
        if (kernel_base > original.base) {
            mm[at] = original;
            mm[at].length = kernel_base - original.base;
            at++;
        }
        mm[at].base = kernel_base;
        mm[at].length = kernel_size;
        mm[at].type = KBI_MEM_KERNEL;
        mm[at].attributes = 0;
        at++;
        if (kernel_end < original_end) {
            mm[at] = original;
            mm[at].base = kernel_end;
            mm[at].length = original_end - kernel_end;
        }
        count += extra;
    }
    uint32_t merged = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (merged) {
            kbi_mmap_entry_t *p = &mm[merged - 1];
            if (p->base + p->length > mm[i].base) return -5;
            if (p->base + p->length == mm[i].base && p->type == mm[i].type) {
                if (p->length > UINT64_MAX - mm[i].length) return -4;
                p->length += mm[i].length;
                continue;
            }
        }
        mm[merged++] = mm[i];
    }
    if (!merged) return -7;
    h->mmap_count = merged;
    h->total_size = h->mmap_off + merged * (uint32_t)sizeof(kbi_mmap_entry_t);
    int rc = kbi_validate(blob, h->total_size);
    if (rc) return -10 + rc;
    *out_size = h->total_size;
    return 0;
}
