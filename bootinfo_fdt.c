#include "bootinfo.h"
#include "fdt.h"

#define KBI_FDT_MAX_REGIONS 64u
#define KBI_FDT_MAX_RESERVED 128u

typedef struct { uint64_t base, size; } fdt_range_t;

static int add_range(fdt_range_t* a, uint32_t* n, uint32_t cap, uint64_t base, uint64_t size) {
    if (!size || base > UINT64_MAX - size || *n >= cap) return -1;
    a[*n] = (fdt_range_t){ base, size };
    (*n)++;
    return 0;
}
static int add_region(kbi_mmap_entry_t* a, uint32_t* n, uint64_t base, uint64_t size) {
    if (!size || base > UINT64_MAX - size || *n >= KBI_FDT_MAX_REGIONS) return -1;
    a[*n] = (kbi_mmap_entry_t){ .base=base, .length=size, .type=KBI_MEM_USABLE, .attributes=0 };
    (*n)++;
    return 0;
}
static int reserve_node_ranges(const void* fdt, int node, fdt_range_t* reserved, uint32_t* nr) {
    uint32_t len = 0;
    const void* reg = fdt_prop(fdt, node, "reg", &len);
    if (!reg) {
        /* Dynamic reserved-memory allocations must be placed by the loader before handoff. */
        return fdt_prop(fdt, node, "size", &len) ? -1 : 0;
    }
    int parent = fdt_parent(fdt, node);
    uint32_t ac = 2, sc = 1, stride;
    if (parent < 0 || fdt_prop_u32(fdt, parent, "#address-cells", &ac) ||
        fdt_prop_u32(fdt, parent, "#size-cells", &sc) ||
        ac < 1 || ac > 2 || sc < 1 || sc > 2) return -1;
    stride = 4u * (ac + sc);
    if (!len || len % stride) return -1;
    for (uint32_t i = 0; i < len / stride; i++) {
        uint64_t base, size;
        if (fdt_reg(fdt, node, i, &base, &size) ||
            add_range(reserved, nr, KBI_FDT_MAX_RESERVED, base, size)) return -1;
    }
    return 0;
}
static int subtract_reserved(kbi_mmap_entry_t* regions, uint32_t* count,
                             const fdt_range_t* reserved, uint32_t nr) {
    kbi_mmap_entry_t next[KBI_FDT_MAX_REGIONS];
    for (uint32_t r = 0; r < nr; r++) {
        uint32_t nn = 0;
        uint64_t rb = reserved[r].base, re = rb + reserved[r].size;
        for (uint32_t i = 0; i < *count; i++) {
            uint64_t b = regions[i].base, e = b + regions[i].length;
            if (re <= b || rb >= e) {
                if (nn >= KBI_FDT_MAX_REGIONS) return -1;
                next[nn++] = regions[i];
                continue;
            }
            if (rb > b) {
                if (nn >= KBI_FDT_MAX_REGIONS) return -1;
                next[nn] = regions[i]; next[nn].length = rb - b; nn++;
            }
            if (re < e) {
                if (nn >= KBI_FDT_MAX_REGIONS) return -1;
                next[nn] = regions[i]; next[nn].base = re; next[nn].length = e - re; nn++;
            }
        }
        for (uint32_t i = 0; i < nn; i++) regions[i] = next[i];
        *count = nn;
    }
    return 0;
}

/* Build the common KBI handoff from an ARM Device Tree. Memory banks are sorted
   and coalesced so overlapping firmware descriptions cannot double-count RAM. */
int kbi_from_fdt(const void* fdt, uint32_t available, uint64_t fdt_phys,
                 uint64_t kernel_base, uint64_t kernel_size,
                 void* out, uint32_t capacity, uint32_t* out_size) {
    if (out_size) *out_size = 0;
    if (!fdt || !out || !out_size || !fdt_phys || (fdt_phys & 7) ||
        !kernel_base || !kernel_size || kernel_base > UINT64_MAX - kernel_size ||
        !fdt_tree_valid(fdt, available)) return -1;

    kbi_mmap_entry_t regions[KBI_FDT_MAX_REGIONS];
    uint32_t count = 0;
    int depth = 0;
    int root = fdt_root(fdt);
    if (root < 0) return -2;
    fdt_range_t reserved[KBI_FDT_MAX_RESERVED];
    uint32_t nr = 0;
    unsigned nfirmware = fdt_reserve_count(fdt, available);
    for (unsigned i = 0; i < nfirmware; i++) {
        uint64_t base, size;
        if (fdt_reserve(fdt, available, i, &base, &size) ||
            add_range(reserved, &nr, KBI_FDT_MAX_RESERVED, base, size)) return -4;
    }
    /* The kernel image and DTB are live boot objects, not free allocator pages. */
    if (add_range(reserved, &nr, KBI_FDT_MAX_RESERVED, kernel_base, kernel_size) ||
        add_range(reserved, &nr, KBI_FDT_MAX_RESERVED, fdt_phys, fdt_total_size(fdt))) return -4;
    int reserved_root = fdt_find_path(fdt, "/reserved-memory");
    if (reserved_root >= 0) {
        uint32_t ranges_len = 0;
        const void* ranges = fdt_prop(fdt, reserved_root, "ranges", &ranges_len);
        if (!ranges || ranges_len != 0) return -4;
        int rd = 0;
        for (int node = fdt_next_node(fdt, reserved_root, &rd); node >= 0;
             node = fdt_next_node(fdt, node, &rd)) {
            if (fdt_parent(fdt, node) != reserved_root) continue;
            if (!fdt_node_enabled(fdt, node)) continue;
            if (reserve_node_ranges(fdt, node, reserved, &nr)) return -4;
        }
    }
    for (int node = root; node >= 0; node = fdt_next_node(fdt, node, &depth)) {
        if (!fdt_node_enabled(fdt, node)) continue;
        uint32_t type_len = 0;
        const char* type = (const char*)fdt_prop(fdt, node, "device_type", &type_len);
        if (!type || type_len < 7 || __builtin_memcmp(type, "memory", 7)) continue;

        int parent = fdt_parent(fdt, node);
        uint32_t ac = 2, sc = 1, reg_len = 0;
        if (parent >= 0) {
            fdt_prop_u32(fdt, parent, "#address-cells", &ac);
            fdt_prop_u32(fdt, parent, "#size-cells", &sc);
        }
        const void* reg = fdt_prop(fdt, node, "reg", &reg_len);
        if (ac < 1 || ac > 2 || sc < 1 || sc > 2 || !reg) return -3;
        uint32_t stride = 4 * (ac + sc);
        if (!stride || !reg_len || reg_len % stride) return -3;
        for (uint32_t i = 0; i < reg_len / stride; i++) {
            uint64_t base = 0, size = 0;
            if (fdt_reg(fdt, node, i, &base, &size)) return -3;
            if (size && add_region(regions, &count, base, size)) return -4;
        }
    }
    if (!count) return -5;
    fdt_fb_t reserved_fb;
    if (fdt_simplefb(fdt, &reserved_fb) == 0 &&
        add_range(reserved, &nr, KBI_FDT_MAX_RESERVED, reserved_fb.base, reserved_fb.size)) return -4;

    /* Insertion sort by physical base. */
    for (uint32_t i = 1; i < count; i++) {
        kbi_mmap_entry_t v = regions[i];
        uint32_t j = i;
        while (j && regions[j - 1].base > v.base) { regions[j] = regions[j - 1]; j--; }
        regions[j] = v;
    }
    /* Merge overlap and adjacency. All end calculations were overflow-checked above. */
    uint32_t merged = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t end = regions[i].base + regions[i].length;
        if (merged) {
            kbi_mmap_entry_t* prev = &regions[merged - 1];
            uint64_t prev_end = prev->base + prev->length;
            if (regions[i].base <= prev_end) {
                if (end > prev_end) prev->length = end - prev->base;
                continue;
            }
        }
        regions[merged++] = regions[i];
    }
    if (subtract_reserved(regions, &merged, reserved, nr) != 0 || !merged) return -4;

    uint32_t cmd_len = 0;
    const char* cmd = 0;
    int chosen = fdt_find_path(fdt, "/chosen");
    if (chosen >= 0) {
        uint32_t n = 0;
        cmd = (const char*)fdt_prop(fdt, chosen, "bootargs", &n);
        if (cmd && n) {
            while (cmd_len < n && cmd[cmd_len]) cmd_len++;
            if (cmd_len == n || cmd_len + 1 != n) return -6; /* exactly one terminated bootargs string */
            cmd_len++;
        } else cmd = 0;
    }

    uint64_t need64 = sizeof(kbi_header_t) + (uint64_t)merged * sizeof(kbi_mmap_entry_t) + cmd_len;
    if (need64 > capacity || need64 > KIRA_BOOTINFO_MAX_SIZE || need64 > UINT32_MAX) return -7;
    uint32_t total = (uint32_t)need64;
    __builtin_memset(out, 0, total);

    kbi_header_t* h = (kbi_header_t*)out;
    h->magic = KIRA_BOOTINFO_MAGIC;
    h->version = KIRA_BOOTINFO_VERSION;
    h->header_size = sizeof(*h);
    h->total_size = total;
    h->flags = KBI_F_FDT | (cmd_len ? KBI_F_CMDLINE : 0);
    h->arch = KBI_ARCH_AARCH64;
    h->boot_mode = KBI_BOOT_DEVICE_TREE;
    h->mmap_off = sizeof(*h);
    h->mmap_count = merged;
    h->mmap_entry_size = sizeof(kbi_mmap_entry_t);
    h->fdt_phys = fdt_phys;
    h->kernel_phys_base = kernel_base;
    h->kernel_size = kernel_size;
    h->loader_id = FDT_MAGIC;

    kbi_mmap_entry_t* dst = (kbi_mmap_entry_t*)((uint8_t*)out + h->mmap_off);
    for (uint32_t i = 0; i < merged; i++) dst[i] = regions[i];
    if (cmd_len) {
        h->cmdline_off = h->mmap_off + merged * sizeof(kbi_mmap_entry_t);
        h->cmdline_len = cmd_len;
        __builtin_memcpy((uint8_t*)out + h->cmdline_off, cmd, cmd_len);
    }

    fdt_fb_t fb;
    if (fdt_simplefb(fdt, &fb) == 0) {
        h->flags |= KBI_F_FRAMEBUFFER;
        h->fb_phys = fb.base;
        h->fb_size = fb.size;
        h->fb_width = fb.width;
        h->fb_height = fb.height;
        h->fb_pitch = fb.stride;
        h->fb_bpp = 32;
        h->fb_red_pos = 16; h->fb_red_size = 8;
        h->fb_green_pos = 8; h->fb_green_size = 8;
        h->fb_blue_pos = 0; h->fb_blue_size = 8;
    }

    if (kbi_validate(out, total)) return -8;
    *out_size = total;
    return 0;
}
