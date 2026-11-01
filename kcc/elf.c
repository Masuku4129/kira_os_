/* kcc: раскладка и запись исполняемого ELF Киры (x86-64, ET_EXEC).
 * Формат как у userland/user.ld и проверки elf_parse() ядра: база 0x8000000000, три сегмента PT_LOAD на границах страниц -
 * text R-X, rodata R--, data RW- (.data, затем .bss как хвост без файла); W+X нет; заголовки лежат в странице 0 файла и не
 * отображаются. Ссылки разрешаются здесь же (статическая линковка единственного модуля). */
#include "x86.h"
#include <string.h>

#define BASE 0x8000000000ULL
#define PAGE 4096ULL

static uint64_t alup(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
static void w16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void w32(unsigned char *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (unsigned char)(v >> (8 * i)); }
static void w64(unsigned char *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }

static void phdr(unsigned char *p, uint32_t flags, uint64_t off, uint64_t va, uint64_t fsz, uint64_t msz) {
    w32(p, 1); w32(p + 4, flags); w64(p + 8, off); w64(p + 16, va); w64(p + 24, va); w64(p + 32, fsz); w64(p + 40, msz); w64(p + 48, PAGE);
}

void elf_emit(Obj *globals, XA *xa, unsigned char **out, size_t *outlen) {
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
    uint64_t textlen = xa->len;
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
    memcpy(img + text_foff, xa->p, (size_t)textlen);
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
    for (XFix *f = xa->fix; f; f = f->next) {
        Obj *t = f->target;
        if (!t->emit) kerr(src, f->line, "undefined reference to '%s'", t->name);
        int64_t v = (int64_t)t->addr + f->add - (int64_t)(BASE + (uint64_t)f->off + 4);
        if (v < INT32_MIN || v > INT32_MAX) kerr(src, f->line, "relocation to '%s' is out of range", t->name);
        w32(img + text_foff + (uint64_t)f->off, (uint32_t)v);
    }
    /* 4. заголовки */
    unsigned nph = 1 + (rolen ? 1 : 0) + ((data_msz ? 1 : 0));
    img[0] = 0x7F; img[1] = 'E'; img[2] = 'L'; img[3] = 'F'; img[4] = 2; img[5] = 1; img[6] = 1;
    w16(img + 16, 2); w16(img + 18, 62); w32(img + 20, 1);
    w64(img + 24, BASE); w64(img + 32, 64); w64(img + 40, 0);
    w16(img + 52, 64); w16(img + 54, 56); w16(img + 56, (uint16_t)nph); w16(img + 58, 0); w16(img + 60, 0); w16(img + 62, 0);
    unsigned char *ph = img + 64;
    phdr(ph, 5, text_foff, BASE, textlen, textlen); ph += 56;
    if (rolen) { phdr(ph, 4, ro_foff, ro_va, rolen, rolen); ph += 56; }
    if (data_msz) phdr(ph, 6, data_foff, data_va, dlen, data_msz);
    *out = img;
    *outlen = (size_t)total;
}
