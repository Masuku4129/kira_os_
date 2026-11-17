#include "kaslr.h"

const char* kaslr_reason(int r) {
    switch (r) {
    case KASLR_OK:           return "slide chosen";
    case KASLR_OFF_CMDLINE:  return "disabled by kira.nokaslr";
    case KASLR_NO_TABLE:     return "image has no relocation table";
    case KASLR_BAD_BOOTINFO: return "boot information unusable";
    case KASLR_NO_SLOT:      return "no free 2 MB slot below 1 GB";
    case KASLR_BAD_TABLE:    return "relocation table is damaged";
    default:                 return "not run";
    }
}

int kaslr_cmdline_has(const char* cmdline, uint32_t len, const char* word) {
    if (!cmdline || !word || !*word) return 0;
    uint32_t wl = 0;
    while (word[wl]) wl++;
    uint32_t i = 0;
    while (i < len && cmdline[i]) {
        while (i < len && (cmdline[i] == ' ' || cmdline[i] == '\t')) i++;
        uint32_t s = i;
        while (i < len && cmdline[i] && cmdline[i] != ' ' && cmdline[i] != '\t') i++;
        if (i - s == wl) {
            uint32_t k = 0;
            while (k < wl && cmdline[s + k] == word[k]) k++;
            if (k == wl) return 1;
        }
    }
    return 0;
}

static int add_range(kaslr_range_t* a, unsigned* n, uint64_t lo, uint64_t hi) {
    if (lo >= hi) return 0;
    if (*n >= KASLR_MAX_RANGES) return -1;
    a[*n].lo = lo; a[*n].hi = hi; (*n)++;
    return 0;
}

// Сортировка вставками по lo, затем слияние пересекающихся и соседних диапазонов.
static void normalize(kaslr_range_t* a, unsigned* n) {
    for (unsigned i = 1; i < *n; i++) {
        kaslr_range_t v = a[i];
        unsigned j = i;
        while (j && a[j - 1].lo > v.lo) { a[j] = a[j - 1]; j--; }
        a[j] = v;
    }
    unsigned w = 0;
    for (unsigned i = 0; i < *n; i++) {
        if (w && a[i].lo <= a[w - 1].hi) { if (a[i].hi > a[w - 1].hi) a[w - 1].hi = a[i].hi; }
        else a[w++] = a[i];
    }
    *n = w;
}

int kaslr_collect(const void* blob, uint32_t size, uint64_t extra_lo, uint64_t extra_hi, kaslr_mem_t* out) {
    if (!blob || !out) return -1;
    out->n_usable = out->n_avoid = 0;
    out->ram_top = 0;
    if (kbi_validate(blob, size) != 0) return -1;
    const kbi_header_t* h = (const kbi_header_t*)blob;
    const kbi_mmap_entry_t* mm = kbi_mmap(blob);
    for (uint32_t i = 0; i < h->mmap_count; i++) {
        uint64_t b = mm[i].base, l = mm[i].length;
        if (!l || b > UINT64_MAX - l || mm[i].type != KBI_MEM_USABLE) continue;
        if (b + l > out->ram_top) out->ram_top = b + l;
        (void)add_range(out->usable, &out->n_usable, b, b + l);     // сверх предела - просто не кандидаты (безопасно)
    }
    const kbi_module_t* md = kbi_modules(blob);
    for (uint32_t i = 0; i < h->modules_count; i++) {
        if (md[i].base > UINT64_MAX - md[i].length) return -1;
        if (add_range(out->avoid, &out->n_avoid, md[i].base, md[i].base + md[i].length) != 0) return -2;
    }
    if (add_range(out->avoid, &out->n_avoid, (uint64_t)(uintptr_t)blob, (uint64_t)(uintptr_t)blob + size) != 0) return -2;
    if (h->kernel_size && h->kernel_phys_base <= UINT64_MAX - h->kernel_size &&
        add_range(out->avoid, &out->n_avoid, h->kernel_phys_base, h->kernel_phys_base + h->kernel_size) != 0) return -2;
    if (h->fb_size && h->fb_phys <= UINT64_MAX - h->fb_size &&
        add_range(out->avoid, &out->n_avoid, h->fb_phys, h->fb_phys + h->fb_size) != 0) return -2;
    if (add_range(out->avoid, &out->n_avoid, extra_lo, extra_hi) != 0) return -2;
    normalize(out->usable, &out->n_usable);
    normalize(out->avoid, &out->n_avoid);
    return 0;
}

static uint64_t align_up(uint64_t v) { return (v + KASLR_ALIGN - 1) & ~(KASLR_ALIGN - 1); }

static int slot_ok(const kaslr_env_t* e, uint64_t b, uint64_t sz) {
    for (unsigned k = 0; k < e->n_avoid; k++)
        if (b < e->avoid[k].hi && e->avoid[k].lo < b + sz) return 0;
    return 1;
}

// Обход подходящих баз: idx_want < 0 - только счёт, иначе в *found - база с этим номером.
static uint64_t walk(const kaslr_env_t* e, int64_t idx_want, uint64_t* found) {
    uint64_t sz = align_up(e->size), count = 0;
    for (unsigned u = 0; u < e->n_usable; u++) {
        uint64_t lo = e->usable[u].lo > e->floor ? e->usable[u].lo : e->floor;
        uint64_t hi = e->usable[u].hi < e->ceil ? e->usable[u].hi : e->ceil;
        if (lo >= hi) continue;
        for (uint64_t b = align_up(lo); b <= UINT64_MAX - sz && b + sz <= hi; b += KASLR_ALIGN) {
            if (!slot_ok(e, b, sz)) continue;
            if (idx_want >= 0 && count == (uint64_t)idx_want) { *found = b; return count + 1; }
            count++;
        }
    }
    return count;
}

uint64_t kaslr_slots(const kaslr_env_t* e) {
    if (!e || !e->size) return 0;
    return walk(e, -1, 0);
}

int kaslr_pick(const kaslr_env_t* e, uint64_t entropy, uint64_t* base, uint64_t* slots) {
    uint64_t n = kaslr_slots(e);
    if (slots) *slots = n;
    if (!n || !base) return -1;
    uint64_t found = 0;
    walk(e, (int64_t)(entropy % n), &found);
    *base = found;
    return 0;
}

uint64_t kaslr_mix(uint64_t acc, uint64_t sample) {
    uint64_t z = acc * 0xFF51AFD7ED558CCDULL + sample + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr64(uint8_t* p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

int kreloc_check(const void* tbl, uint32_t avail, uint32_t image_loaded_size) {
    if (!tbl || avail < sizeof(kreloc_hdr_t)) return -1;
    const uint8_t* t = (const uint8_t*)tbl;
    if (rd32(t) != KRELOC_MAGIC || rd32(t + 4) != KRELOC_VERSION) return -1;
    uint32_t n64 = rd32(t + 8), n32 = rd32(t + 12), ls = rd32(t + 16);
    if (ls != image_loaded_size) return -1;
    uint64_t need = sizeof(kreloc_hdr_t) + 4ULL * n64 + 4ULL * n32;
    if (need > avail) return -1;
    const uint8_t* o = t + sizeof(kreloc_hdr_t);
    for (uint32_t i = 0; i < n64; i++) if ((uint64_t)rd32(o + 4 * i) + 8 > ls) return -1;
    o += 4 * (uint64_t)n64;
    for (uint32_t i = 0; i < n32; i++) if ((uint64_t)rd32(o + 4 * i) + 4 > ls) return -1;
    return 0;
}

int64_t kreloc_apply(uint8_t* dst, uint32_t loaded_size, const void* tbl, uint32_t avail, uint64_t slide) {
    if (kreloc_check(tbl, avail, loaded_size) != 0) return -1;
    const uint8_t* t = (const uint8_t*)tbl;
    uint32_t n64 = rd32(t + 8), n32 = rd32(t + 12);
    const uint8_t* o = t + sizeof(kreloc_hdr_t);
    for (uint32_t i = 0; i < n64; i++) {
        uint8_t* p = dst + rd32(o + 4 * i);
        uint64_t v = (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
        wr64(p, v + slide);
    }
    o += 4 * (uint64_t)n64;
    for (uint32_t i = 0; i < n32; i++) {
        uint8_t* p = dst + rd32(o + 4 * i);
        uint64_t v = (uint64_t)rd32(p) + slide;
        if (v > 0x7FFFFFFFULL) return -1;
        wr32(p, (uint32_t)v);
    }
    return (int64_t)n64 + (int64_t)n32;
}
