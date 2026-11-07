#include "bootinfo.h"
#include <stddef.h>

static uint32_t uefi_mem_type(uint32_t t) {
    switch (t) {
    case 3: case 4: case 7: return KBI_MEM_USABLE; /* BootServices after ExitBootServices; ConventionalMemory */
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
    if (out_size) *out_size = 0;
    if (!descriptors || !out || !out_size ||
        descriptor_size < sizeof(kbi_uefi_memory_desc_t) ||
        map_size == 0 || map_size % descriptor_size ||
        !kernel_base || !kernel_size ||
        (kernel_base & 4095u) || (kernel_size & 4095u) ||
        kernel_base > UINT64_MAX - kernel_size ||
        (rsdp_phys && (rsdp_phys & 15u))) return -1;
    uint32_t n = map_size / descriptor_size;
    if (n > KIRA_BOOTINFO_MAX_MMAP) return -2;
    uint64_t max_entries = (uint64_t)n + 2u;
    /* Reserve a temporary copy after the maximum output map. This permits
       splitting a kernel image span across multiple UEFI descriptors. */
    uint64_t need = sizeof(kbi_header_t) +
                    (max_entries + n) * sizeof(kbi_mmap_entry_t);
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
        if (d->physical_start & 4095u) return -4;
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
    /* Reserve the full kernel image span. PT_LOAD segments may have been
       allocated separately as EFI_LOADER_CODE and EFI_LOADER_DATA; the
       intervening holes stay reserved too and are never returned to PMM. */
    uint64_t kernel_end = kernel_base + kernel_size;
    if (count + 2u > KIRA_BOOTINFO_MAX_MMAP) return -6;
    kbi_mmap_entry_t *source = mm + max_entries;
    for (uint32_t i = 0; i < count; i++) source[i] = mm[i];
    uint32_t out_count = 0;
    uint64_t covered = kernel_base;
    for (uint32_t i = 0; i < count; i++) {
        kbi_mmap_entry_t original = source[i];
        uint64_t original_end = original.base + original.length;
        if (original_end <= kernel_base || original.base >= kernel_end) {
            if (out_count >= max_entries) return -6;
            mm[out_count++] = original;
            continue;
        }
        uint64_t overlap_start = original.base > kernel_base ? original.base : kernel_base;
        uint64_t overlap_end = original_end < kernel_end ? original_end : kernel_end;
        if (overlap_start != covered || overlap_end <= overlap_start) return -6;
        if (original.base < kernel_base) {
            if (out_count >= max_entries) return -6;
            mm[out_count] = original;
            mm[out_count].length = kernel_base - original.base;
            out_count++;
        }
        if (out_count >= max_entries) return -6;
        mm[out_count].base = overlap_start;
        mm[out_count].length = overlap_end - overlap_start;
        mm[out_count].type = KBI_MEM_KERNEL;
        mm[out_count].attributes = 0;
        out_count++;
        if (original_end > kernel_end) {
            if (out_count >= max_entries) return -6;
            mm[out_count] = original;
            mm[out_count].base = kernel_end;
            mm[out_count].length = original_end - kernel_end;
            out_count++;
        }
        covered = overlap_end;
    }
    if (covered != kernel_end || !out_count) return -6;
    count = out_count;
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
