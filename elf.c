#include "elf.h"

#define PAGE 4096ULL

static uint16_t r16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t r64(const uint8_t* p) { return (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32); }

int elf_parse(const void* image, size_t len, uint64_t win_lo, uint64_t win_hi, uint32_t max_pages, elf_info_t* out) {
    const uint8_t* b = (const uint8_t*)image;
    if (!b || !out || len < 64) return ELF_ERR_SHORT;
    if (b[0] != 0x7F || b[1] != 'E' || b[2] != 'L' || b[3] != 'F') return ELF_ERR_MAGIC;
    if (b[4] != 2 || b[5] != 1 || b[6] != 1) return ELF_ERR_FORMAT;                 /* 64 бита, little endian, версия 1 */
#if (defined(__aarch64__) && !defined(KIRA_HOST_TEST)) || defined(KIRA_HOST_ARM64)
#define ELF_MACHINE 183                                                                /* EM_AARCH64 */
#else
#define ELF_MACHINE 62                                                                 /* EM_X86_64 */
#endif
    if (r16(b + 16) != 2 || r16(b + 18) != ELF_MACHINE) return ELF_ERR_FORMAT;      /* ET_EXEC своей архитектуры */
    uint64_t entry = r64(b + 24), phoff = r64(b + 32);
    uint16_t phentsize = r16(b + 54), phnum = r16(b + 56);
    if (phentsize != 56 || phnum == 0 || phnum > 64) return ELF_ERR_FORMAT;
    if (phoff > len || (uint64_t)phnum * 56 > len - phoff) return ELF_ERR_SHORT;

    out->nsegs = 0; out->pages = 0; out->entry = entry;
    int entry_ok = 0;
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t* ph = b + phoff + (uint64_t)i * 56;
        uint32_t type = r32(ph), flags = r32(ph + 4);
        if (type == 0x6474E551) { if (flags & 1) return ELF_ERR_SEGMENT; continue; }  /* GNU_STACK: исполняемый стек не принимается */
        if (type != 1) continue;                                                     /* только PT_LOAD нужен; остальное безвредно */
        uint64_t off = r64(ph + 8), va = r64(ph + 16), fsz = r64(ph + 32), msz = r64(ph + 40);
        if (msz == 0) continue;
        if (out->nsegs >= ELF_MAX_SEGS) return ELF_ERR_LIMIT;
        if ((flags & 2) && (flags & 1)) return ELF_ERR_SEGMENT;                      /* W^X */
        if (!(flags & 4)) return ELF_ERR_SEGMENT;
        if (fsz > msz) return ELF_ERR_SEGMENT;
        if (va & (PAGE - 1) || (off & (PAGE - 1)) != 0) return ELF_ERR_SEGMENT;     /* сегмент начинается на границе страницы и в файле */
        if (off > len || fsz > len - off) return ELF_ERR_SHORT;
        if (va < win_lo || va >= win_hi || msz > win_hi - va) return ELF_ERR_SEGMENT;
        /* Round without addition overflow, then validate the complete mapped
           page range (not only the byte range) against the address window. */
        uint64_t pages = msz / PAGE + (msz % PAGE != 0);
        if (pages > max_pages || out->pages > max_pages ||
            pages > (win_hi - va) / PAGE ||
            pages > (UINT64_MAX - va) / PAGE)
            return ELF_ERR_LIMIT;
        uint64_t end = va + pages * PAGE;
        for (unsigned k = 0; k < out->nsegs; k++) {                                  /* страницы сегментов не пересекаются */
            uint64_t ks = out->seg[k].vaddr;
            uint64_t kpages = out->seg[k].mem_size / PAGE + (out->seg[k].mem_size % PAGE != 0);
            if (kpages > (UINT64_MAX - ks) / PAGE) return ELF_ERR_SEGMENT;
            uint64_t ke = ks + kpages * PAGE;
            if (va < ke && ks < end) return ELF_ERR_SEGMENT;
        }
        if (pages > (uint64_t)(max_pages - out->pages)) return ELF_ERR_LIMIT;
        elf_seg_t* s = &out->seg[out->nsegs++];
        s->vaddr = va; s->file_off = off; s->file_size = fsz; s->mem_size = msz;
        s->write = (flags & 2) != 0; s->exec = (flags & 1) != 0;
        out->pages += (uint32_t)pages;
        if ((flags & 1) && entry >= va && entry < va + msz) entry_ok = 1;
    }
    if (out->nsegs == 0) return ELF_ERR_SEGMENT;
    if (!entry_ok) return ELF_ERR_ENTRY;
    return ELF_OK;
}

const char* elf_strerror(int e) {
    switch (e) {
    case ELF_OK: return "ok";
    case ELF_ERR_SHORT: return "файл обрезан или заголовок выходит за файл";
    case ELF_ERR_MAGIC: return "это не ELF";
    case ELF_ERR_FORMAT: return "не ELF64 x86-64 для запуска (нужен ET_EXEC)";
    case ELF_ERR_SEGMENT: return "недопустимый сегмент (адрес, W+X, пересечение или размеры)";
    case ELF_ERR_ENTRY: return "точка входа вне исполняемого сегмента";
    case ELF_ERR_LIMIT: return "программа слишком велика";
    default: return "ошибка";
    }
}
