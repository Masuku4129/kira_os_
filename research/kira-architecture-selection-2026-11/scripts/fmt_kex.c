#include "fmt_kex.h"
#include "../../../immune.h"
#define PAGE 4096ULL
static uint32_t r32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t r64(const uint8_t* p) { return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32); }
static uint16_t r16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
int kex_parse(const void* image, size_t len, uint64_t win_lo, uint64_t win_hi, uint32_t max_pages, elf_info_t* out) {
    const uint8_t* b = (const uint8_t*)image;
    if (!b || len < KEX_HDR) return KEX_ERR_SHORT;
    if (r32(b) != KEX_MAGIC) return KEX_ERR_MAGIC;
    if (r16(b + 4) != 1 || r16(b + 6) != KEX_HDR || r32(b + 8) != len || r32(b + 36) != 0) return KEX_ERR_FORMAT;
    for (unsigned i = 40; i < 64; i++) if (b[i]) return KEX_ERR_FORMAT;
#if (defined(__aarch64__) && !defined(KIRA_HOST_TEST)) || defined(KIRA_HOST_ARM64)
    if (r16(b + 12) != 183) return KEX_ERR_FORMAT;
#else
    if (r16(b + 12) != 62) return KEX_ERR_FORMAT;
#endif
    unsigned nseg = r16(b + 14); uint64_t entry = r64(b + 16); uint32_t nrel = r32(b + 28), roff = r32(b + 32);
    if (nseg == 0) return KEX_ERR_SEGMENT;
    if (nseg > ELF_MAX_SEGS) return KEX_ERR_LIMIT;
    if (r32(b + 24) != KEX_HDR) return KEX_ERR_FORMAT;
    uint64_t tab_end = KEX_HDR + (uint64_t)nseg * KEX_SEG;
    if (nrel > KEX_MAX_RELOC) return KEX_ERR_LIMIT;
    if (roff != tab_end || (uint64_t)nrel * KEX_REL > len - tab_end) return KEX_ERR_SHORT;
    uint64_t data_start = tab_end + (uint64_t)nrel * KEX_REL, cursor = data_start;
    out->nsegs = 0; out->pages = 0; out->entry = entry;
    int entry_ok = 0;
    for (unsigned i = 0; i < nseg; i++) {
        const uint8_t* s = b + KEX_HDR + (uint64_t)i * KEX_SEG;
        uint64_t va = r64(s), off = r32(s + 8), fsz = r32(s + 12), msz = r32(s + 16); uint32_t fl = r32(s + 20);
        if (fl & ~3u) return KEX_ERR_SEGMENT;
        if ((fl & 3u) == 3u) return KEX_ERR_SEGMENT;                                /* W^X */
        if (msz == 0 || fsz > msz) return KEX_ERR_SEGMENT;
        if (va & (PAGE - 1)) return KEX_ERR_SEGMENT;
        if (off != cursor || fsz > len - off) return KEX_ERR_SHORT;                 /* данные подряд, без дыр и наложений */
        cursor += fsz;
        if (va < win_lo || va >= win_hi || msz > win_hi - va) return KEX_ERR_SEGMENT;
        uint64_t pages = (msz + PAGE - 1) / PAGE;
        if (pages > max_pages || out->pages + pages > max_pages) return KEX_ERR_LIMIT;
        uint64_t end = va + pages * PAGE;
        for (unsigned k = 0; k < out->nsegs; k++) {
            uint64_t ks = out->seg[k].vaddr, ke = ks + ((out->seg[k].mem_size + PAGE - 1) / PAGE) * PAGE;
            if (va < ke && ks < end) return KEX_ERR_SEGMENT;
        }
        elf_seg_t* o = &out->seg[out->nsegs++];
        o->vaddr = va; o->file_off = off; o->file_size = fsz; o->mem_size = msz; o->write = (fl & 2) != 0; o->exec = (fl & 1) != 0;
        out->pages += (uint32_t)pages;
        if ((fl & 1) && entry >= va && entry < va + msz) entry_ok = 1;
    }
    if (cursor != len) return KEX_ERR_SHORT;                                         /* хвоста нет */
    if (!entry_ok) return KEX_ERR_ENTRY;
    for (uint32_t i = 0; i < nrel; i++) {                                           /* перемещение: слово целиком внутри файловой части сегмента */
        const uint8_t* e = b + roff + (uint64_t)i * KEX_REL; uint64_t at = out->seg[0].vaddr + r32(e); int ok = 0;
        if (r32(e + 4) != 1) return KEX_ERR_RELOC;
        for (unsigned k = 0; k < out->nsegs; k++) { const elf_seg_t* g = &out->seg[k]; if (at >= g->vaddr && g->file_size >= 8 && at <= g->vaddr + g->file_size - 8) { ok = 1; break; } }
        if (!ok) return KEX_ERR_RELOC;
    }
    return KEX_OK;
}
int kex_verify_hash(const void* image, size_t len) {
    const uint8_t* b = (const uint8_t*)image; uint8_t head[KEX_HDR], d[32]; sha256_ctx_t c;
    if (len < KEX_HDR) return KEX_ERR_SHORT;
    for (unsigned i = 0; i < KEX_HDR; i++) head[i] = (i >= 64 && i < 96) ? 0 : b[i];
    sha256_init(&c); sha256_update(&c, head, KEX_HDR); sha256_update(&c, b + KEX_HDR, len - KEX_HDR); sha256_final(&c, d);
    uint8_t x = 0; for (unsigned i = 0; i < 32; i++) x |= (uint8_t)(d[i] ^ b[64 + i]);
    return x ? KEX_ERR_HASH : KEX_OK;
}
