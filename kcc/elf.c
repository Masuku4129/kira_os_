/* kcc: раскладка и запись исполняемого ELF Киры (x86-64, ET_EXEC).
 * Формат как у userland/user.ld и проверки elf_parse() ядра: база 0x8000000000, три сегмента PT_LOAD на границах страниц -
 * text R-X, rodata R--, data RW- (.data, затем .bss как хвост без файла); W+X нет; заголовки лежат в странице 0 файла и не
 * отображаются. Ссылки разрешаются здесь же (статическая линковка единственного модуля). */
#include "target.h"
#include <string.h>

#define BASE_X86 0x8000000000ULL
#define BASE_A64 0x808000000000ULL
#define PAGE 4096ULL

static uint64_t alup(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
static void w16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void w32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static void w64(unsigned char *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }

static void phdr(unsigned char *p, uint32_t flags, uint64_t off, uint64_t va, uint64_t fsz, uint64_t msz) {
    w32(p, 1); w32(p + 4, flags); w64(p + 8, off); w64(p + 16, va); w64(p + 24, va); w64(p + 32, fsz); w64(p + 40, msz); w64(p + 48, PAGE);
}

uint64_t elf_base(int arch) { return arch == ELF_ARCH_A64 ? BASE_A64 : BASE_X86; }

void elf_emit(Obj *globals, const unsigned char *code, size_t codelen, int arch, void (*reloc)(unsigned char *text, void *ctx), void *ctx,
              unsigned char **out, size_t *outlen) {
    const uint64_t BASE = elf_base(arch);
    const char *src = K.srcpath;
    /* 1. раскладка данных: rodata, data, bss */
    uint64_t rolen = 0, dlen = 0, blen = 0;
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 2) blen = dlen;              /* bss продолжает сегмент data */
        for (Obj *g = globals; g; g = g->next) {
            if (g->is_func || !g->emit || !g->is_def) continue;
            int kind = g->init ? (g->is_ro ? 0 : 1) : 2;
            if (kind != pass) continue;
            int al = g->align > g->ty->align ? g->align : g->ty->align;
            if (al < 1) al = 1;
            if (al > (int)PAGE) kerr(src, g->line, "unsupported alignment %d of '%s'", al, g->name);
            uint64_t *len = pass == 0 ? &rolen : pass == 1 ? &dlen : &blen;
            uint64_t sz = g->ty->size > 0 ? (uint64_t)g->ty->size : 0;
            if (sz > (1ULL << 30) || *len > (1ULL << 30)) kerr(src, g->line, "data of '%s' is too large", g->name);
            *len = alup(*len, (uint64_t)al);
            g->addr = *len;                      /* пока смещение внутри секции */
            *len += sz;
        }
    }
    uint64_t textlen = codelen;
    uint64_t ro_va = alup(BASE + textlen, PAGE);
    uint64_t data_va = alup(ro_va + rolen, PAGE);
    uint64_t data_msz = blen;
    uint64_t text_foff = PAGE, ro_foff = text_foff + alup(textlen, PAGE), data_foff = ro_foff + alup(rolen, PAGE);
    uint64_t total = data_foff + dlen;
    if (total > (1ULL << 30)) kerr(src, 1, "output image is too large");
    unsigned char *img = ka_new((size_t)total + 1);
    /* 2. адреса символов */
    for (Obj *g = globals; g; g = g->next) {
        if (!g->emit) continue;
        if (g->is_func) { g->addr += BASE; continue; }
        if (!g->is_def) continue;
        int kind = g->init ? (g->is_ro ? 0 : 1) : 2;
        g->addr += kind == 0 ? ro_va : data_va;
    }
    /* 3. содержимое */
    memcpy(img + text_foff, code, (size_t)textlen);
    for (Obj *g = globals; g; g = g->next) {
        if (g->is_func || !g->emit || !g->is_def || !g->init) continue;
        uint64_t va = g->addr, fo = g->is_ro ? ro_foff + (va - ro_va) : data_foff + (va - data_va);
        memcpy(img + fo, g->init, g->ty->size > 0 ? (size_t)g->ty->size : 0);
        for (Reloc *r = g->relocs; r; r = r->next) {
            Obj *t = gen_lookup(r->sym);
            if (!t || !t->emit) kerr(src, g->line, "undefined reference to '%s'", r->sym);
            w64(img + fo + (uint64_t)r->off, t->addr + (uint64_t)r->addend);
        }
    }
    reloc(img + text_foff, ctx);
    /* 4. заголовки */
    unsigned nph = 1 + (rolen ? 1 : 0) + ((data_msz ? 1 : 0));
    img[0] = 0x7F; img[1] = 'E'; img[2] = 'L'; img[3] = 'F'; img[4] = 2; img[5] = 1; img[6] = 1;
    w16(img + 16, 2); w16(img + 18, arch == ELF_ARCH_A64 ? 183 : 62); w32(img + 20, 1);
    w64(img + 24, BASE); w64(img + 32, 64); w64(img + 40, 0);
    w16(img + 52, 64); w16(img + 54, 56); w16(img + 56, (uint16_t)nph); w16(img + 58, 0); w16(img + 60, 0); w16(img + 62, 0);
    unsigned char *ph = img + 64;
    phdr(ph, 5, text_foff, BASE, textlen, textlen); ph += 56;
    if (rolen) { phdr(ph, 4, ro_foff, ro_va, rolen, rolen); ph += 56; }
    if (data_msz) phdr(ph, 6, data_foff, data_va, dlen, data_msz);
    *out = img;
    *outlen = (size_t)total;
}

/* ---------- объектный файл ET_REL (x86-64) ---------- */
typedef struct { unsigned char *p; size_t len, cap; } OBuf;
static size_t ob_put(OBuf *b, const void *d, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n) nc *= 2;
        unsigned char *np = ka_new(nc);
        if (b->len) memcpy(np, b->p, b->len);
        b->p = np; b->cap = nc;
    }
    if (d) memcpy(b->p + b->len, d, n); else memset(b->p + b->len, 0, n);
    b->len += n;
    return b->len - n;
}
static size_t ob_str(OBuf *b, const char *s) { return ob_put(b, s, strlen(s) + 1); }
typedef struct { unsigned char b[24]; } Sym;
static Sym mksym(uint32_t name, int info, int shndx, uint64_t val, uint64_t size) {
    Sym s; memset(&s, 0, sizeof s);
    w32(s.b, name); s.b[4] = (unsigned char)info; w16(s.b + 6, (uint16_t)shndx); w64(s.b + 8, val); w64(s.b + 16, size);
    return s;
}
static void mkrela(OBuf *b, uint64_t off, uint32_t sym, uint32_t type, int64_t add) {
    unsigned char r[24];
    w64(r, off); w64(r + 8, ((uint64_t)sym << 32) | type); w64(r + 16, (uint64_t)add);
    ob_put(b, r, 24);
}
static void shdr(unsigned char *p, uint32_t name, uint32_t type, uint64_t flags, uint64_t off, uint64_t size, uint32_t link, uint32_t info,
                 uint64_t align, uint64_t entsize) {
    w32(p, name); w32(p + 4, type); w64(p + 8, flags); w64(p + 16, 0); w64(p + 24, off); w64(p + 32, size);
    w32(p + 40, link); w32(p + 44, info); w64(p + 48, align); w64(p + 56, entsize);
}

void elf_emit_obj(Obj *globals, const unsigned char *code, size_t codelen, int arch, const ObjRel *rel, size_t nrel,
                  unsigned char **out, size_t *outlen) {
    const char *src = K.srcpath;
    if (arch != ELF_ARCH_X86) kerr(src, 1, "object output is only implemented for x86_64");
    enum { S_TEXT = 1, S_RO, S_DATA, S_BSS, S_RELT, S_RELR, S_RELD, S_SYMTAB, S_STRTAB, S_SHSTR, S_NOTE, S_COUNT };
    /* 1. раскладка данных по секциям, как в exec-режиме; kind: 0 rodata, 1 data, 2 bss */
    uint64_t len[3] = { 0, 0, 0 }, aln[3] = { 1, 1, 1 };
    for (int pass = 0; pass < 3; pass++)
        for (Obj *g = globals; g; g = g->next) {
            if (g->is_func || !g->emit || !g->is_def) continue;
            int kind = g->init ? (g->is_ro ? 0 : 1) : 2;
            if (kind != pass) continue;
            int al = g->align > g->ty->align ? g->align : g->ty->align;
            if (al < 1) al = 1;
            if (al > (int)PAGE) kerr(src, g->line, "unsupported alignment %d of '%s'", al, g->name);
            if ((uint64_t)al > aln[pass]) aln[pass] = (uint64_t)al;
            uint64_t sz = g->ty->size > 0 ? (uint64_t)g->ty->size : 0;
            len[pass] = alup(len[pass], (uint64_t)al);
            g->addr = len[pass];
            len[pass] += sz;
        }
    /* 2. символы: сначала локальные, потом глобальные и неопределённые */
    OBuf strtab = {0}, symtab = {0};
    ob_put(&strtab, NULL, 1);
    ob_put(&symtab, NULL, 24);
    int nsym = 1, nlocal = 1;
    int cnt = 0;
    for (Obj *g = globals; g; g = g->next) cnt++;
    uint32_t *symidx = ka_new(sizeof(uint32_t) * (size_t)(cnt + 1));
    Obj **lst = ka_new(sizeof(Obj *) * (size_t)(cnt + 1));
    int k = 0;
    for (Obj *g = globals; g; g = g->next) lst[k++] = g;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < cnt; i++) {
            Obj *g = lst[i];
            if (!g->emit || g->is_local || g->is_param) continue;
            if (gen_lookup(g->name) != g && gen_is_defined(g)) continue;
            int def = g->is_func ? g->body != NULL : g->is_def;
            int local = def && (g->is_static || g->name[0] == '.' || g->is_str);
            if ((pass == 0) != local) continue;
            int kind = g->is_func ? 0 : (g->init ? (g->is_ro ? 0 : 1) : 2);
            int shndx = !def ? 0 : g->is_func ? S_TEXT : kind == 0 ? S_RO : kind == 1 ? S_DATA : S_BSS;
            int type = g->is_func ? 2 : 1;
            int info = ((local ? 0 : 1) << 4) | (def ? type : 0);
            uint64_t sz = def ? (g->is_func ? 0 : (g->ty->size > 0 ? (uint64_t)g->ty->size : 0)) : 0;
            size_t nm = ob_str(&strtab, g->name);
            Sym sy = mksym((uint32_t)nm, info, shndx, def ? g->addr : 0, sz);
            ob_put(&symtab, sy.b, 24);
            symidx[i] = (uint32_t)nsym++;
            if (pass == 0) nlocal = nsym;
        }
    /* соответствие Obj -> индекс символа: линейный поиск по списку (внутри gen_lookup возвращается определение) */
    #define SYMOF(o, var) do { (var) = 0; for (int q = 0; q < cnt; q++) if (lst[q] == (o)) { (var) = symidx[q]; break; } } while (0)
    /* 3. переносы */
    OBuf relt = {0}, relr = {0}, reld = {0};
    for (size_t i = 0; i < nrel; i++) {
        Obj *t = rel[i].t;
        Obj *d = gen_lookup(t->name);
        if (d) t = d;
        uint32_t si; SYMOF(t, si);
        if (!si) kerr(src, 1, "internal: no symbol for '%s'", t->name);
        mkrela(&relt, rel[i].off, si, (uint32_t)rel[i].type, rel[i].add);
    }
    for (Obj *g = globals; g; g = g->next) {
        if (g->is_func || !g->emit || !g->is_def || !g->init) continue;
        OBuf *rb = g->is_ro ? &relr : &reld;
        for (Reloc *r = g->relocs; r; r = r->next) {
            Obj *t = gen_lookup(r->sym);
            if (!t) kerr(src, g->line, "undefined reference to '%s'", r->sym);
            uint32_t si; SYMOF(t, si);
            if (!si) kerr(src, g->line, "internal: no symbol for '%s'", r->sym);
            mkrela(rb, g->addr + (uint64_t)r->off, si, 1, r->addend);
        }
    }
    /* 4. содержимое секций и таблица секций */
    static const char *const shn[S_COUNT] = { "", ".text", ".rodata", ".data", ".bss", ".rela.text", ".rela.rodata", ".rela.data",
                                              ".symtab", ".strtab", ".shstrtab", ".note.GNU-stack" };
    char shnames[160]; uint32_t shnoff[S_COUNT]; size_t shnlen = 0;
    for (int q = 0; q < S_COUNT; q++) { shnoff[q] = (uint32_t)shnlen; size_t l = strlen(shn[q]) + 1; memcpy(shnames + shnlen, shn[q], l); shnlen += l; }
    #define NM(i) shnoff[i]
    OBuf o = {0};
    ob_put(&o, NULL, 64);
    size_t off[S_COUNT], size[S_COUNT];
    memset(off, 0, sizeof off); memset(size, 0, sizeof size);
    #define SECT(i, al) do { size_t a_ = (al) > 1 ? (al) : 1; ob_put(&o, NULL, (a_ - o.len % a_) % a_); off[i] = o.len; } while (0)
    SECT(S_TEXT, 16); ob_put(&o, code, codelen); size[S_TEXT] = codelen;
    SECT(S_RO, aln[0]);
    { size_t base = o.len; ob_put(&o, NULL, (size_t)len[0]);
      for (Obj *g = globals; g; g = g->next)
          if (!g->is_func && g->emit && g->is_def && g->init && g->is_ro) memcpy(o.p + base + g->addr, g->init, g->ty->size > 0 ? (size_t)g->ty->size : 0);
      size[S_RO] = (size_t)len[0]; }
    SECT(S_DATA, aln[1]);
    { size_t base = o.len; ob_put(&o, NULL, (size_t)len[1]);
      for (Obj *g = globals; g; g = g->next)
          if (!g->is_func && g->emit && g->is_def && g->init && !g->is_ro) memcpy(o.p + base + g->addr, g->init, g->ty->size > 0 ? (size_t)g->ty->size : 0);
      size[S_DATA] = (size_t)len[1]; }
    off[S_BSS] = o.len; size[S_BSS] = (size_t)len[2];
    SECT(S_RELT, 8); ob_put(&o, relt.p, relt.len); size[S_RELT] = relt.len;
    SECT(S_RELR, 8); ob_put(&o, relr.p, relr.len); size[S_RELR] = relr.len;
    SECT(S_RELD, 8); ob_put(&o, reld.p, reld.len); size[S_RELD] = reld.len;
    SECT(S_SYMTAB, 8); ob_put(&o, symtab.p, symtab.len); size[S_SYMTAB] = symtab.len;
    SECT(S_STRTAB, 1); ob_put(&o, strtab.p, strtab.len); size[S_STRTAB] = strtab.len;
    SECT(S_SHSTR, 1); ob_put(&o, shnames, shnlen); size[S_SHSTR] = shnlen;
    off[S_NOTE] = o.len;
    SECT(0, 8);
    size_t shoff = o.len;
    ob_put(&o, NULL, 64 * S_COUNT);
    unsigned char *sh = o.p + shoff;
    shdr(sh + 64 * S_TEXT, NM(S_TEXT), 1, 6, off[S_TEXT], size[S_TEXT], 0, 0, 16, 0);
    shdr(sh + 64 * S_RO, NM(S_RO), 1, 2, off[S_RO], size[S_RO], 0, 0, aln[0], 0);
    shdr(sh + 64 * S_DATA, NM(S_DATA), 1, 3, off[S_DATA], size[S_DATA], 0, 0, aln[1], 0);
    shdr(sh + 64 * S_BSS, NM(S_BSS), 8, 3, off[S_BSS], size[S_BSS], 0, 0, aln[2], 0);
    shdr(sh + 64 * S_RELT, NM(S_RELT), 4, 0x40, off[S_RELT], size[S_RELT], S_SYMTAB, S_TEXT, 8, 24);
    shdr(sh + 64 * S_RELR, NM(S_RELR), 4, 0x40, off[S_RELR], size[S_RELR], S_SYMTAB, S_RO, 8, 24);
    shdr(sh + 64 * S_RELD, NM(S_RELD), 4, 0x40, off[S_RELD], size[S_RELD], S_SYMTAB, S_DATA, 8, 24);
    shdr(sh + 64 * S_SYMTAB, NM(S_SYMTAB), 2, 0, off[S_SYMTAB], size[S_SYMTAB], S_STRTAB, (uint32_t)nlocal, 8, 24);
    shdr(sh + 64 * S_STRTAB, NM(S_STRTAB), 3, 0, off[S_STRTAB], size[S_STRTAB], 0, 0, 1, 0);
    shdr(sh + 64 * S_SHSTR, NM(S_SHSTR), 3, 0, off[S_SHSTR], size[S_SHSTR], 0, 0, 1, 0);
    shdr(sh + 64 * S_NOTE, NM(S_NOTE), 1, 0, off[S_NOTE], 0, 0, 0, 1, 0);
    unsigned char *h = o.p;
    h[0] = 0x7F; h[1] = 'E'; h[2] = 'L'; h[3] = 'F'; h[4] = 2; h[5] = 1; h[6] = 1;
    w16(h + 16, 1); w16(h + 18, 62); w32(h + 20, 1);
    w64(h + 40, shoff); w16(h + 52, 64); w16(h + 58, 64); w16(h + 60, S_COUNT); w16(h + 62, S_SHSTR);
    *out = o.p; *outlen = o.len;
}
