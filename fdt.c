#include "fdt.h"

/* Заголовок: magic, totalsize, off_dt_struct, off_dt_strings, off_mem_rsvmap, version, last_comp_version,
   boot_cpuid_phys, size_dt_strings, size_dt_struct (все - 32 бита, большой порядок). */
enum { T_BEGIN = 1, T_END = 2, T_PROP = 3, T_NOP = 4, T_FINISH = 9 };

uint32_t fdt_u32(const void* p) {
    if (!p) return 0;
    const uint8_t* b = (const uint8_t*)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
static uint32_t hdr(const void* fdt, unsigned i) { return fdt_u32((const uint8_t*)fdt + 4 * i); }
static const uint8_t* sblk(const void* fdt) { return (const uint8_t*)fdt + hdr(fdt, 2); }
static uint32_t slen(const void* fdt) { return hdr(fdt, 9); }
static const char* strs(const void* fdt) { return (const char*)fdt + hdr(fdt, 3); }

static int streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static unsigned slen0(const char* s, unsigned cap) { unsigned n = 0; while (n < cap && s[n]) n++; return n; }

static int ranges_overlap_u32(uint32_t a, uint32_t an, uint32_t b, uint32_t bn) {
    return an && bn && a < b + bn && b < a + an;
}
static uint64_t fdt_u64(const uint8_t* p) {
    return ((uint64_t)fdt_u32(p) << 32) | fdt_u32(p + 4);
}

int fdt_valid(const void* fdt, uint32_t max_len) {
    /* Do not read even the magic until the caller's advertised buffer can
       contain the complete fixed-size FDT header. */
    if (!fdt || max_len < 40 || hdr(fdt, 0) != FDT_MAGIC) return 0;
    uint32_t total = hdr(fdt, 1), so = hdr(fdt, 2), sto = hdr(fdt, 3);
    uint32_t rso = hdr(fdt, 4), ver = hdr(fdt, 5);
    uint32_t strings_size = hdr(fdt, 8), struct_size = hdr(fdt, 9);
    /* Tree walkers expose signed int offsets; reject totals that cannot be represented before any offset cast. */
    if (total < 56 || total > INT32_MAX || (total & 3) || total > max_len || ver < 16) return 0;
    if (so < 40 || sto < 40 || rso < 40 || (so & 3) || (sto & 3) || (rso & 7) ||
        so > total || struct_size < 4 || struct_size > total - so ||
        sto > total || strings_size > total - sto || rso > total - 16) return 0;
    if (ranges_overlap_u32(so, struct_size, sto, strings_size)) return 0;

    /* The reserve map is a bounded sequence of 64-bit address/size pairs terminated
       by (0,0). It must not alias either parser-visible block. */
    uint32_t reserve_end = 0;
    const uint8_t* b = (const uint8_t*)fdt;
    for (uint32_t p = rso; p <= total - 16; p += 16) {
        uint64_t a = fdt_u64(b + p), n = fdt_u64(b + p + 8);
        if (!a && !n) { reserve_end = p + 16; break; }
        if (!n || a > UINT64_MAX - n) return 0;
    }
    if (!reserve_end || ranges_overlap_u32(rso, reserve_end - rso, so, struct_size) ||
        ranges_overlap_u32(rso, reserve_end - rso, sto, strings_size)) return 0;
    return 1;
}

uint32_t fdt_total_size(const void* fdt) { return fdt ? hdr(fdt, 1) : 0; }

unsigned fdt_reserve_count(const void* fdt, uint32_t max_len) {
    if (!fdt_valid(fdt, max_len)) return 0;
    uint32_t total = hdr(fdt, 1), off = hdr(fdt, 4);
    const uint8_t* b = (const uint8_t*)fdt;
    unsigned count = 0;
    for (uint32_t p = off; p <= total - 16; p += 16) {
        uint64_t a = fdt_u64(b + p), n = fdt_u64(b + p + 8);
        if (!a && !n) return count;
        count++;
    }
    return 0;
}

int fdt_reserve(const void* fdt, uint32_t max_len, unsigned index, uint64_t* base, uint64_t* size) {
    if (!fdt_valid(fdt, max_len)) return -1;
    uint32_t total = hdr(fdt, 1), off = hdr(fdt, 4);
    const uint8_t* b = (const uint8_t*)fdt;
    unsigned current = 0;
    for (uint32_t p = off; p <= total - 16; p += 16) {
        uint64_t a = fdt_u64(b + p), n = fdt_u64(b + p + 8);
        if (!a && !n) return -1;
        if (current++ == index) {
            if (base) *base = a;
            if (size) *size = n;
            return 0;
        }
    }
    return -1;
}

/* Смещение следующего токена после токена в off; токен в *tok. -1 - выход за блок. */
static int step(const void* fdt, int off, uint32_t* tok) {
    uint32_t n = slen(fdt);
    if (off < 0 || (uint32_t)off + 4 > n) return -1;
    const uint8_t* s = sblk(fdt);
    uint32_t t = fdt_u32(s + off);
    *tok = t;
    off += 4;
    if (t == T_BEGIN) {
        unsigned l = slen0((const char*)s + off, n - (uint32_t)off);
        if ((uint32_t)off + l >= n) return -1;
        off += (int)((l + 1 + 3) & ~3u);
    } else if (t == T_PROP) {
        if ((uint32_t)off + 8 > n) return -1;
        uint32_t len = fdt_u32(s + off);
        if (len > n - (uint32_t)off - 8) return -1;
        off += 8 + (int)((len + 3) & ~3u);
    } else if (t != T_END && t != T_NOP && t != T_FINISH) return -1;
    return off;
}

int fdt_tree_valid(const void* fdt, uint32_t max_len) {
    if (!fdt_valid(fdt, max_len)) return 0;
    const uint8_t* s = sblk(fdt);
    const char* strings = strs(fdt);
    uint32_t n = slen(fdt), strings_size = hdr(fdt, 8), off = 0;
    unsigned depth = 0;
    uint8_t child_seen[64] = {0};
    int root_seen = 0, root_closed = 0, finished = 0;
    while (off < n) {
        uint32_t tok;
        int next = step(fdt, (int)off, &tok);
        if (next < 0 || (uint32_t)next <= off) return 0;
        if (tok == T_BEGIN) {
            if (finished || root_closed) return 0;
            unsigned name_len = slen0((const char*)s + off + 4, n - off - 4);
            if (!root_seen) {
                if (depth || name_len) return 0;
                root_seen = 1;
            } else if (!depth || !name_len) return 0;
            if (depth >= 64) return 0;
            if (depth) child_seen[depth - 1] = 1;
            child_seen[depth] = 0;
            depth++;
        } else if (tok == T_PROP) {
            if (!root_seen || !depth || root_closed || child_seen[depth - 1]) return 0;
            uint32_t name_off = fdt_u32(s + off + 8);
            if (name_off >= strings_size) return 0;
            unsigned name_len = slen0(strings + name_off, strings_size - name_off);
            if (!name_len || name_len == strings_size - name_off) return 0;
        } else if (tok == T_END) {
            if (!root_seen || !depth || root_closed) return 0;
            depth--;
            if (!depth) root_closed = 1;
        } else if (tok == T_FINISH) {
            if (!root_seen || !root_closed || depth || (uint32_t)next != n) return 0;
            finished = 1;
        } else if (tok != T_NOP) return 0;
        if (finished && (uint32_t)next != n) return 0;
        off = (uint32_t)next;
    }
    return finished && root_seen && root_closed && depth == 0;
}

int fdt_root(const void* fdt) {
    if (!fdt) return -1;
    uint32_t tok; int off = 0;
    while (off >= 0) {
        int nx = step(fdt, off, &tok);
        if (nx < 0) return -1;
        if (tok == T_BEGIN) return off;
        if (tok != T_NOP) return -1;
        off = nx;
    }
    return -1;
}

int fdt_next_node(const void* fdt, int node, int* depth) {
    if (!fdt || node < 0 || (depth && *depth < 0)) return -1;
    uint32_t tok;
    int off = step(fdt, node, &tok);           /* за BEGIN текущего узла */
    if (off < 0 || tok != T_BEGIN) return -1;
    int d = depth ? *depth : 0;
    if (d > 64) return -1;
    for (;;) {
        int nx = step(fdt, off, &tok);
        if (nx < 0) return -1;
        if (tok == T_BEGIN) { if (d >= 64) return -1; if (depth) *depth = d + 1; return off; }
        if (tok == T_END) { d--; if (d < -1) return -1; }
        if (tok == T_FINISH) return -1;
        off = nx;
    }
}

const char* fdt_node_name(const void* fdt, int node) {
    uint32_t tok;
    if (!fdt || node < 0 || step(fdt, node, &tok) < 0 || tok != T_BEGIN) return 0;
    return (const char*)sblk(fdt) + node + 4;
}

const void* fdt_prop(const void* fdt, int node, const char* name, uint32_t* len) {
    if (!fdt || node < 0 || !name || !*name) return 0;
    uint32_t tok;
    int off = step(fdt, node, &tok);
    if (off < 0 || tok != T_BEGIN) return 0;
    const uint8_t* s = sblk(fdt);
    uint32_t stsz = hdr(fdt, 8);
    for (;;) {
        int nx = step(fdt, off, &tok);
        if (nx < 0) return 0;
        if (tok == T_PROP) {
            uint32_t l = fdt_u32(s + off + 4), no = fdt_u32(s + off + 8);
            if (no < stsz) {
                const char* candidate = strs(fdt) + no;
                uint32_t avail = stsz - no, i = 0;
                while (i < avail && candidate[i] && name[i] && candidate[i] == name[i]) i++;
                if (i < avail && !candidate[i] && !name[i]) { if (len) *len = l; return s + off + 12; }
            }
        } else if (tok != T_NOP) return 0;     /* свойства идут до первого дочернего узла */
        off = nx;
    }
}

int fdt_compatible(const void* fdt, int node, const char* compat) {
    if (!fdt || node < 0 || !compat || !*compat) return 0;
    uint32_t len;
    const char* c = (const char*)fdt_prop(fdt, node, "compatible", &len);
    if (!c) return 0;
    uint32_t i = 0;
    while (i < len) {                          /* список строк через \0 */
        unsigned l = slen0(c + i, len - i);
        if (i + l < len && streq(c + i, compat)) return 1;
        i += l + 1;
    }
    return 0;
}

int fdt_find_compatible(const void* fdt, int after, const char* compat) {
    if (!fdt || after < -1 || !compat || !*compat) return -1;
    int depth = 0;
    int n = after < 0 ? fdt_root(fdt) : fdt_next_node(fdt, after, &depth);
    while (n >= 0) {
        if (fdt_compatible(fdt, n, compat)) return n;
        n = fdt_next_node(fdt, n, &depth);
    }
    return -1;
}

/* Сравнение одного шага пути с именем узла: «cpus» совпадает с «cpus» и с «cpus@0». */
static int name_match(const char* node, const char* part, unsigned plen) {
    unsigned i = 0;
    while (i < plen && node[i] == part[i]) i++;
    if (i < plen) return 0;
    return node[i] == 0 || (node[i] == '@' && !(i < plen));
}

int fdt_find_path(const void* fdt, const char* path) {
    if (!fdt || !path || path[0] != '/') return -1;
    int node = fdt_root(fdt);
    if (node < 0) return -1;
    const char* p = path + 1;
    while (*p) {
        unsigned plen = 0; while (p[plen] && p[plen] != '/') plen++;
        if (!plen) return -1; /* reject repeated/trailing separators instead of matching an empty name */
        /* среди прямых детей node */
        int depth = 0, found = -1;
        int c = fdt_next_node(fdt, node, &depth);
        while (c >= 0 && depth > 0) {
            if (depth == 1 && name_match(fdt_node_name(fdt, c), p, plen)) { found = c; break; }
            c = fdt_next_node(fdt, c, &depth);
        }
        if (found < 0) return -1;
        node = found;
        p += plen;
        if (*p == '/') { p++; if (!*p) return -1; }
    }
    return node;
}

int fdt_parent(const void* fdt, int node) {
    if (!fdt || node < 0) return -1;
    /* fdt_tree_valid() accepts a maximum nesting depth of 64. Keep the
       parent walk's capacity aligned with that parser limit. */
    int stack[64]; int depth = 0;
    int n = fdt_root(fdt);
    if (n < 0 || n == node) return -1;
    stack[0] = n;
    while (n >= 0) {
        int d = depth;
        n = fdt_next_node(fdt, n, &d);
        if (n < 0) return -1;
        if (d <= 0 || d >= 64) return -1;
        stack[d] = n;
        depth = d;
        if (n == node) return stack[d - 1];
    }
    return -1;
}

int fdt_prop_u32(const void* fdt, int node, const char* name, uint32_t* out) {
    if (!fdt || node < 0 || !name || !*name || !out) return -1;
    uint32_t len;
    const void* p = fdt_prop(fdt, node, name, &len);
    if (!p || len != 4) return -1;
    *out = fdt_u32(p);
    return 0;
}

int fdt_reg(const void* fdt, uint32_t max_len, int node, unsigned idx, uint64_t* addr, uint64_t* size) {
    if (!fdt || node < 0 || (!addr && !size)) return -1;
    /* A malformed property length can make fdt_prop() stop before finding a
       present cell-width property; without validating the token stream that
       would be mistaken for an absent property and trigger a default. */
    if (!fdt_tree_valid(fdt, max_len)) return -1;
    int par = fdt_parent(fdt, node);
    uint32_t ac = 2, sc = 1;                   /* значения по умолчанию спецификации */
    if (par >= 0) {
        uint32_t cell_len = 0;
        const void* cell = fdt_prop(fdt, par, "#address-cells", &cell_len);
        if (cell) { if (cell_len != 4) return -1; ac = fdt_u32(cell); }
        cell = fdt_prop(fdt, par, "#size-cells", &cell_len);
        if (cell) { if (cell_len != 4) return -1; sc = fdt_u32(cell); }
    }
    if (ac < 1 || ac > 2 || sc > 2) return -1;
    uint32_t len;
    const uint8_t* r = (const uint8_t*)fdt_prop(fdt, node, "reg", &len);
    uint32_t stride = 4 * (ac + sc);
    if (!r || ((uint64_t)idx + 1) * stride > len) return -1;
    r += (uint64_t)idx * stride;
    uint64_t a = 0, s = 0;
    for (uint32_t i = 0; i < ac; i++) a = (a << 32) | fdt_u32(r + 4 * i);
    for (uint32_t i = 0; i < sc; i++) s = (s << 32) | fdt_u32(r + 4 * (ac + i));
    if (s && a > UINT64_MAX - s) return -1;
    if (addr) *addr = a;
    if (size) *size = s;
    return 0;
}

unsigned fdt_count_type(const void* fdt, int parent, const char* type) {
    if (!fdt || parent < 0 || !type || !*type) return 0;
    unsigned n = 0;
    int depth = 0;
    int c = fdt_next_node(fdt, parent, &depth);
    while (c >= 0 && depth > 0) {
        if (depth == 1) {
            uint32_t len;
            const char* t = (const char*)fdt_prop(fdt, c, "device_type", &len);
            unsigned want = slen0(type, 256);
            if (want < 256 && t && len == want + 1 && !__builtin_memcmp(t, type, want) && t[want] == 0) n++;
        }
        c = fdt_next_node(fdt, c, &depth);
    }
    return n;
}


int fdt_node_enabled(const void* fdt, int node) {
    if (!fdt || node < 0) return 0;
    uint32_t len;
    const char* s = (const char*)fdt_prop(fdt, node, "status", &len);
    if (!s) return 1;
    if (len == 5 && !__builtin_memcmp(s, "okay", 4) && s[4] == 0) return 1;
    if (len == 3 && s[0] == 'o' && s[1] == 'k' && s[2] == 0) return 1;
    return 0;
}

int fdt_find_enabled_compatible(const void* fdt, int after, const char* compat) {
    int node = after;
    while ((node = fdt_find_compatible(fdt, node, compat)) >= 0)
        if (fdt_node_enabled(fdt, node)) return node;
    return -1;
}

int fdt_simplefb(const void* fdt, uint32_t max_len, fdt_fb_t* fb) {
    if (!fdt || !fb || !fdt_tree_valid(fdt, max_len)) return -2;
    int n = -1;
    do n = fdt_find_compatible(fdt, n, "simple-framebuffer"); while (n >= 0 && !fdt_node_enabled(fdt, n));
    if (n < 0) return -1;
    uint32_t len;
    const char* f = (const char*)fdt_prop(fdt, n, "format", &len);
    if (fdt_reg(fdt, max_len, n, 0, &fb->base, &fb->size) || fdt_prop_u32(fdt, n, "width", &fb->width) ||
        fdt_prop_u32(fdt, n, "height", &fb->height) || fdt_prop_u32(fdt, n, "stride", &fb->stride) || !f || !len) return -2;
    unsigned k = slen0(f, len);
    if (k >= sizeof fb->format || k + 1 != len) return -2;
    for (unsigned i = 0; i < k; i++) fb->format[i] = f[i];
    fb->format[k] = 0;
    uint64_t min_stride = (uint64_t)fb->width * 4;
    if (!fb->width || !fb->height || min_stride > UINT32_MAX ||
        fb->stride < min_stride || (uint64_t)fb->stride * fb->height > fb->size) return -2;
    if (!streq(fb->format, "x8r8g8b8") && !streq(fb->format, "a8r8g8b8")) return -3;
    return 0;
}