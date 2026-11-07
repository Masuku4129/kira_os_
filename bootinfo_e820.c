#include "bootinfo.h"
#include <stddef.h>

static uint32_t e820_type(uint32_t type) {
    switch (type) {
    case 1: return KBI_MEM_USABLE;
    case 3: return KBI_MEM_ACPI_RECLAIM;
    case 4: return KBI_MEM_ACPI_NVS;
    case 5: return KBI_MEM_BAD;
    default: return KBI_MEM_RESERVED;
    }
}

int kbi_from_e820_map(const kbi_e820_entry_t *entries, uint32_t n,
                      uint64_t kernel_base, uint64_t kernel_size,
                      void *out, uint32_t capacity, uint32_t *out_size) {
    if (!entries || !n || n > KIRA_BOOTINFO_MAX_MMAP || !out || !out_size ||
        !kernel_base || !kernel_size || kernel_base > UINT64_MAX - kernel_size)
        return -1;
    uint64_t max_count = (uint64_t)n + 2u;
    uint64_t need = sizeof(kbi_header_t) + max_count * sizeof(kbi_mmap_entry_t);
    if (need > capacity || need > KIRA_BOOTINFO_MAX_SIZE) return -2;
    uint8_t *blob = out;
    for (uint64_t i = 0; i < need; i++) blob[i] = 0;
    kbi_header_t *h = (kbi_header_t *)blob;
    h->magic = KIRA_BOOTINFO_MAGIC; h->version = KIRA_BOOTINFO_VERSION;
    h->header_size = sizeof(*h); h->arch = KBI_ARCH_X86_64;
    h->boot_mode = KBI_BOOT_BIOS; h->mmap_off = sizeof(*h);
    h->mmap_entry_size = sizeof(kbi_mmap_entry_t);
    h->kernel_phys_base = kernel_base; h->kernel_size = kernel_size;
    h->loader_id = 0x30323845u; /* "E820" */
    kbi_mmap_entry_t *mm = (kbi_mmap_entry_t *)(blob + h->mmap_off);
    uint32_t count = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!entries[i].length) continue;
        if (entries[i].base > UINT64_MAX - entries[i].length) return -3;
        mm[count].base = entries[i].base;
        mm[count].length = entries[i].length;
        mm[count].type = e820_type(entries[i].type);
        mm[count].attributes = entries[i].attributes;
        count++;
    }
    if (!count) return -4;
    for (uint32_t i = 1; i < count; i++) {
        kbi_mmap_entry_t v = mm[i]; uint32_t j = i;
        while (j && mm[j - 1].base > v.base) { mm[j] = mm[j - 1]; j--; }
        mm[j] = v;
    }
    uint64_t kend = kernel_base + kernel_size;
    uint32_t owner = count;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t end = mm[i].base + mm[i].length;
        if (kernel_base >= mm[i].base && kend <= end) { owner = i; break; }
    }
    if (owner == count || count + 2u > KIRA_BOOTINFO_MAX_MMAP) return -5;
    kbi_mmap_entry_t original = mm[owner];
    uint64_t original_end = original.base + original.length;
    uint32_t extra = (kernel_base > original.base) + (kend < original_end);
    for (uint32_t i = count; i > owner + 1u; i--)
        mm[i + extra - 1u] = mm[i - 1u];
    uint32_t at = owner;
    if (kernel_base > original.base) {
        mm[at] = original; mm[at].length = kernel_base - original.base; at++;
    }
    mm[at].base = kernel_base; mm[at].length = kernel_size;
    mm[at].type = KBI_MEM_KERNEL; mm[at].attributes = 0; at++;
    if (kend < original_end) {
        mm[at] = original; mm[at].base = kend; mm[at].length = original_end - kend;
    }
    count += extra;
    uint32_t merged = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (merged) {
            kbi_mmap_entry_t *p = &mm[merged - 1];
            if (p->base + p->length > mm[i].base) return -6;
            if (p->base + p->length == mm[i].base && p->type == mm[i].type) {
                if (p->length > UINT64_MAX - mm[i].length) return -3;
                p->length += mm[i].length; continue;
            }
        }
        mm[merged++] = mm[i];
    }
    h->mmap_count = merged;
    h->total_size = h->mmap_off + merged * (uint32_t)sizeof(kbi_mmap_entry_t);
    int rc = kbi_validate(blob, h->total_size);
    if (rc) return -10 + rc;
    *out_size = h->total_size;
    return 0;
}
