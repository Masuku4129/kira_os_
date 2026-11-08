/* klink: компоновщик x86-64. ET_REL (clang, nasm, kcc) -> ET_EXEC по скрипту в подмножестве GNU ld.
 *
 * Этапы: разбор объектов -> разрешение символов -> разбор скрипта -> привязка входных секций к выходным ->
 * --gc-sections -> раскладка адресов -> программные заголовки -> перемещения -> запись ELF.
 * Всё неподдержанное - явная ошибка с именем файла, секции и символа. Память - арена из KLink (malloc не используется). */
#include "klink.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define SHF_WRITE 1ULL
#define SHF_ALLOC 2ULL
#define SHF_EXEC 4ULL
#define SHF_MERGE 0x10ULL
#define SHF_GROUP 0x200ULL
#define SHF_TLS 0x400ULL
#define SHF_COMPRESSED 0x800ULL
enum { SHT_NULL = 0, SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_RELA = 4, SHT_NOTE = 7, SHT_NOBITS = 8, SHT_REL = 9,
       SHT_INIT_ARRAY = 14, SHT_FINI_ARRAY = 15, SHT_PREINIT_ARRAY = 16, SHT_GROUP = 17, SHT_X64_UNWIND = 0x70000001,
       SHT_LLVM_ADDRSIG = 0x6fff4c03 };
enum { SHN_UNDEF = 0, SHN_ABS = 0xfff1, SHN_COMMON = 0xfff2, SHN_XINDEX = 0xffff };
enum { STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_SECTION = 3, STT_FILE = 4 };
enum { STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2 };
enum { R_NONE = 0, R_64 = 1, R_PC32 = 2, R_GOT32 = 3, R_PLT32 = 4, R_GOTPCREL = 9, R_32 = 10, R_32S = 11, R_PC64 = 24, R_GOTPCRELX = 41,
       R_REX_GOTPCRELX = 42 };
enum { GS_UNDEF = 0, GS_DEF = 1, GS_COMMON = 2, GS_SCRIPT = 3 };

#define MAX_SEC_SIZE (1ULL << 31)
#define MAX_ALIGN (1ULL << 20)
#define MAX_ADDR (1ULL << 47)
#define MAX_IMAGE (1ULL << 30)
#define MAX_PHDRS 16

typedef struct KObj KObj;
typedef struct KSec KSec;
typedef struct GSym GSym;
typedef struct KOut KOut;
typedef struct SStmt SStmt;

typedef struct {
    const char *name;
    uint8_t info, other;
    uint32_t shndx;
    uint64_t value, size;
    GSym *g;
} KSym;

struct KSec {
    KObj *obj;
    uint32_t idx, type;
    uint64_t flags, align, size;
    const char *name;
    const uint8_t *data;             /* NULL для NOBITS */
    const uint8_t *rela;
    uint32_t nrela;
    SStmt *stmt;
    KSec *snext;
    KOut *out;
    uint64_t addr, off;              /* адрес в памяти; смещение внутри выходной секции */
    uint8_t cand, live, discard;
};

struct KObj {
    const char *name;
    const uint8_t *d;
    size_t n;
    uint32_t nsec, nsym;
    KSec *sec;
    KSym *sym;
};

struct GSym {
    const char *name;
    GSym *hnext, *lnext;
    uint8_t state, weak, type;
    uint8_t hidden;                  /* STV_HIDDEN/INTERNAL: в таблице выхода становится локальным, как у ld.lld */
    uint8_t wref;                    /* на неопределённый символ есть слабая ссылка */
    uint8_t inside;                  /* символ скрипта: задан внутри тела выходной секции */
    KObj *obj;
    KSec *sec;
    uint64_t value, size, calign;
    KOut *owner;
};

struct KOut {
    const char *name;
    KOut *next;
    uint64_t va, size, off, align;
    uint32_t flags;                  /* SHF_* */
    int nobits, nonempty, shndx, seg;
    SStmt *st, *phst;
};

/* ---------- скрипт ---------- */
enum { T_END, T_WORD, T_NUM, T_PUNCT };
typedef struct { int type, line; const char *s; int len; uint64_t num; } Tok;
enum { S_ASSIGN = 1, S_INPUT, S_OUT };
struct SStmt {
    int kind, line;
    SStmt *next;
    const char *name;
    int op, provide, tok;
    const char *file;
    const char **pat;
    int npat, keep;
    KSec *head, *tail;
    SStmt *body;
    int addr_tok, align_tok, discard;
    const char *phn[4];
    int nph;
    KOut *out;
};
typedef struct { const char *name; int has_flags; uint32_t flags; } PhDef;
typedef struct { uint32_t flags; KOut *first, *last; uint64_t va, filesz, memsz, off; } Seg;

typedef struct {
    KLink *k;
    jmp_buf jb;
    unsigned char *ab;
    size_t aused, acap;
    KObj *objs;
    unsigned nobjs;
    GSym **ht;
    size_t htsize;
    GSym *gfirst, *glast;
    KSec *commonsec;
    Tok *tok;
    int ntok;
    const char *entry;
    SStmt *items;                    /* верхний уровень SECTIONS */
    int have_sections;
    PhDef ph[MAX_PHDRS];
    int nph;
    int have_phdrs;
    KOut *outs, *outs_tail;
    unsigned nsec_in, nsec_gc;
    KSec *comment_first;
    uint64_t pagesz;
} Ctx;
static Ctx C;

static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(C.k->err, sizeof C.k->err, fmt, ap);
    va_end(ap);
    longjmp(C.jb, 1);
}

#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || __has_feature(address_sanitizer)
void __asan_poison_memory_region(void const volatile *p, size_t n);
void __asan_unpoison_memory_region(void const volatile *p, size_t n);
#define KL_REDZONE 16                            /* под ASan каждое выделение окружено запрещённой зоной: выход за границы внутри арены виден */
#define KL_POISON(p, n) __asan_poison_memory_region(p, n)
#define KL_UNPOISON(p, n) __asan_unpoison_memory_region(p, n)
#else
#define KL_REDZONE 0
#define KL_POISON(p, n) ((void)(p), (void)(n))
#define KL_UNPOISON(p, n) ((void)(p), (void)(n))
#endif

static void *anew(size_t n) {
    size_t full = ((n + 15) & ~(size_t)15) + KL_REDZONE;
    if (n > ((size_t)1 << 40) || full > C.acap - C.aused) fail("out of memory: arena of %llu bytes is too small", (unsigned long long)C.acap);
    void *p = C.ab + C.aused;
    C.aused += full;
    memset(p, 0, n);
    KL_POISON((char *)p + n, full - n);
    return p;
}

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t r64(const uint8_t *p) { return (uint64_t)r32(p) | (uint64_t)r32(p + 4) << 32; }
static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void w64(uint8_t *p, uint64_t v) { w32(p, (uint32_t)v); w32(p + 4, (uint32_t)(v >> 32)); }
static int inrange(uint64_t off, uint64_t len, uint64_t total) { return off <= total && len <= total - off; }
static uint64_t alup(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

static const char *base_name(const char *p) {
    const char *b = p;
    for (; *p; p++)
        if (*p == '/') b = p + 1;
    return b;
}

/* строка в таблице строк: смещение внутри, завершающий 0 обязателен */
static const char *getstr(const uint8_t *tab, uint64_t tsz, uint64_t off) {
    if (off >= tsz) return NULL;
    for (uint64_t i = off; i < tsz; i++)
        if (tab[i] == 0) return (const char *)tab + off;
    return NULL;
}

/* ---------- хеш глобальных символов ---------- */
static uint64_t hash_name(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ULL;
    return h;
}

static GSym *gfind(const char *name) {
    for (GSym *g = C.ht[hash_name(name) & (C.htsize - 1)]; g; g = g->hnext)
        if (strcmp(g->name, name) == 0) return g;
    return NULL;
}

static GSym *gget(const char *name) {
    GSym *g = gfind(name);
    if (g) return g;
    g = anew(sizeof *g);
    g->name = name;
    size_t b = hash_name(name) & (C.htsize - 1);
    g->hnext = C.ht[b];
    C.ht[b] = g;
    if (C.glast) C.glast->lnext = g; else C.gfirst = g;
    C.glast = g;
    return g;
}

/* ---------- разбор ET_REL ---------- */
static const char *typename_rel(uint32_t t) {
    switch (t) {
    case R_NONE: return "R_X86_64_NONE";
    case R_64: return "R_X86_64_64";
    case R_PC32: return "R_X86_64_PC32";
    case R_PLT32: return "R_X86_64_PLT32";
    case R_32: return "R_X86_64_32";
    case R_32S: return "R_X86_64_32S";
    case R_PC64: return "R_X86_64_PC64";
    case R_GOT32: return "R_X86_64_GOT32";
    case R_GOTPCREL: return "R_X86_64_GOTPCREL";
    case 5: return "R_X86_64_COPY";
    case 6: return "R_X86_64_GLOB_DAT";
    case 7: return "R_X86_64_JUMP_SLOT";
    case 8: return "R_X86_64_RELATIVE";
    case 12: return "R_X86_64_16";
    case 13: return "R_X86_64_PC16";
    case 14: return "R_X86_64_8";
    case 15: return "R_X86_64_PC8";
    case 16: return "R_X86_64_DTPMOD64";
    case 17: return "R_X86_64_DTPOFF64";
    case 18: return "R_X86_64_TPOFF64";
    case 19: return "R_X86_64_TLSGD";
    case 20: return "R_X86_64_TLSLD";
    case 21: return "R_X86_64_DTPOFF32";
    case 22: return "R_X86_64_GOTTPOFF";
    case 23: return "R_X86_64_TPOFF32";
    case 25: return "R_X86_64_GOTOFF64";
    case R_GOTPCRELX: return "R_X86_64_GOTPCRELX";
    case R_REX_GOTPCRELX: return "R_X86_64_REX_GOTPCRELX";
    case 26: return "R_X86_64_GOTPC32";
    case 32: return "R_X86_64_SIZE32";
    case 33: return "R_X86_64_SIZE64";
    case 34: return "R_X86_64_GOTPC32_TLSDESC";
    case 35: return "R_X86_64_TLSDESC_CALL";
    case 36: return "R_X86_64_TLSDESC";
    case 37: return "R_X86_64_IRELATIVE";
    default: return "unknown";
    }
}

static void parse_obj(KObj *o) {
    const uint8_t *d = o->d;
    size_t n = o->n;
    if (n >= 8 && memcmp(d, "!<arch>\n", 8) == 0) fail("%s: archives (.a) are not supported; pass object files", o->name);
    if (n < 64 || d[0] != 0x7f || d[1] != 'E' || d[2] != 'L' || d[3] != 'F') fail("%s: not an ELF file", o->name);
    if (d[4] != 2 || d[5] != 1 || d[6] != 1) fail("%s: not a little-endian ELF64 file", o->name);
    if (r16(d + 16) != 1) fail("%s: not a relocatable object (e_type=%u)", o->name, r16(d + 16));
    if (r16(d + 18) != 62) fail("%s: not an x86-64 object (e_machine=%u)", o->name, r16(d + 18));
    uint64_t shoff = r64(d + 40);
    unsigned shnum = r16(d + 60), shstrndx = r16(d + 62);
    if (r16(d + 58) != 64) fail("%s: bad section header entry size", o->name);
    if (shnum == 0 || shnum >= 0xff00) fail("%s: unsupported section count %u", o->name, shnum);
    if (!inrange(shoff, (uint64_t)shnum * 64, n)) fail("%s: section header table is outside the file", o->name);
    if (shstrndx >= shnum) fail("%s: bad e_shstrndx", o->name);
    o->nsec = shnum;
    o->sec = anew(sizeof(KSec) * shnum);
    const uint8_t *shstr = NULL;
    uint64_t shstrsz = 0;
    {
        const uint8_t *h = d + shoff + (uint64_t)shstrndx * 64;
        if (r32(h + 4) != SHT_STRTAB || !inrange(r64(h + 24), r64(h + 32), n)) fail("%s: bad section name table", o->name);
        shstr = d + r64(h + 24);
        shstrsz = r64(h + 32);
    }
    int symtab = -1;
    for (unsigned i = 0; i < shnum; i++) {
        const uint8_t *h = d + shoff + (uint64_t)i * 64;
        KSec *s = &o->sec[i];
        s->obj = o;
        s->idx = i;
        s->type = r32(h + 4);
        s->flags = r64(h + 8);
        s->size = r64(h + 32);
        s->align = r64(h + 48);
        s->name = getstr(shstr, shstrsz, r32(h));
        if (!s->name) fail("%s: section %u has a bad name", o->name, i);
        if (s->align == 0) s->align = 1;
        if ((s->align & (s->align - 1)) != 0 || s->align > MAX_ALIGN) fail("%s: section '%s' has unsupported alignment %llu", o->name, s->name, (unsigned long long)s->align);
        if (s->type != SHT_NOBITS && s->type != SHT_NULL) {
            if (!inrange(r64(h + 24), s->size, n)) fail("%s: section '%s' is outside the file", o->name, s->name);
            s->data = d + r64(h + 24);
        }
        if (s->size > MAX_SEC_SIZE) fail("%s: section '%s' is too large", o->name, s->name);
        if (s->type == SHT_SYMTAB) {
            if (symtab >= 0) fail("%s: more than one symbol table", o->name);
            symtab = (int)i;
        }
        if (s->type == SHT_REL) fail("%s: section '%s': SHT_REL relocations are not supported (only RELA)", o->name, s->name);
        if (s->type == SHT_GROUP || (s->flags & SHF_GROUP)) fail("%s: section '%s': COMDAT/section groups are not supported", o->name, s->name);
        if (s->flags & SHF_TLS) fail("%s: section '%s': thread-local storage is not supported", o->name, s->name);
        if (s->flags & SHF_COMPRESSED) fail("%s: section '%s': compressed sections are not supported", o->name, s->name);
        if (s->flags & SHF_ALLOC) {
            switch (s->type) {
            case SHT_PROGBITS: case SHT_NOBITS: case SHT_NOTE: case SHT_X64_UNWIND: case SHT_INIT_ARRAY: case SHT_FINI_ARRAY: case SHT_PREINIT_ARRAY:
                s->cand = 1;
                break;
            default:
                fail("%s: allocatable section '%s' has unsupported type 0x%x", o->name, s->name, s->type);
            }
        }
    }
    /* таблица символов */
    if (symtab >= 0) {
        const uint8_t *h = d + shoff + (uint64_t)symtab * 64;
        uint32_t link = r32(h + 40);
        if (link >= shnum || o->sec[link].type != SHT_STRTAB) fail("%s: symbol table has no string table", o->name);
        if (r64(h + 56) != 24 || o->sec[symtab].size % 24) fail("%s: bad symbol table entry size", o->name);
        const uint8_t *str = o->sec[link].data;
        uint64_t strsz = o->sec[link].size;
        o->nsym = (uint32_t)(o->sec[symtab].size / 24);
        o->sym = anew(sizeof(KSym) * (o->nsym + 1));
        for (uint32_t i = 0; i < o->nsym; i++) {
            const uint8_t *e = o->sec[symtab].data + (uint64_t)i * 24;
            KSym *y = &o->sym[i];
            y->name = getstr(str, strsz, r32(e));
            if (!y->name) fail("%s: symbol %u has a bad name", o->name, i);
            y->info = e[4];
            y->other = e[5];
            y->shndx = r16(e + 6);
            y->value = r64(e + 8);
            y->size = r64(e + 16);
            if (i == 0) continue;
            unsigned type = y->info & 15, bind = y->info >> 4;
            if (type > STT_FILE) fail("%s: symbol '%s' has unsupported type %u (TLS and IFUNC are not supported)", o->name, y->name, type);
            if (bind > STB_WEAK) fail("%s: symbol '%s' has unsupported binding %u", o->name, y->name, bind);
            if (y->shndx == SHN_XINDEX) fail("%s: symbol '%s': extended section indices are not supported", o->name, y->name);
            if (y->shndx >= 0xff00 && y->shndx != SHN_ABS && y->shndx != SHN_COMMON) fail("%s: symbol '%s' has unsupported section index 0x%x", o->name, y->name, y->shndx);
            if (y->shndx < 0xff00 && y->shndx >= shnum) fail("%s: symbol '%s' refers to a missing section %u", o->name, y->name, y->shndx);
            if (y->shndx == SHN_COMMON && (y->value == 0 || (y->value & (y->value - 1)) || y->value > MAX_ALIGN || y->size > MAX_SEC_SIZE))
                fail("%s: common symbol '%s' has a bad alignment or size", o->name, y->name);
            if (y->shndx < 0xff00 && y->shndx != 0 && type != STT_FILE && y->value > o->sec[y->shndx].size)
                fail("%s: symbol '%s' lies outside section '%s'", o->name, y->name, o->sec[y->shndx].name);
        }
    }
    /* перемещения */
    for (unsigned i = 0; i < shnum; i++) {
        KSec *s = &o->sec[i];
        if (s->type != SHT_RELA) continue;
        const uint8_t *h = d + shoff + (uint64_t)i * 64;
        uint32_t tgt = r32(h + 44), link = r32(h + 40);
        if (tgt == 0 || tgt >= shnum || tgt == i) fail("%s: relocation section '%s' has a bad target", o->name, s->name);
        if (symtab < 0 || link != (uint32_t)symtab) fail("%s: relocation section '%s' does not use the symbol table", o->name, s->name);
        if (r64(h + 56) != 24 || s->size % 24) fail("%s: bad relocation entry size in '%s'", o->name, s->name);
        KSec *t = &o->sec[tgt];
        if (t->rela) fail("%s: several relocation sections for '%s'", o->name, t->name);
        if (t->type == SHT_NOBITS && s->size) fail("%s: relocations against NOBITS section '%s'", o->name, t->name);
        t->rela = s->data;
        t->nrela = (uint32_t)(s->size / 24);
        for (uint32_t k = 0; k < t->nrela; k++) {
            const uint8_t *r = t->rela + (uint64_t)k * 24;
            if (r64(r) > t->size) fail("%s: relocation %u in '%s' lies outside the section", o->name, k, t->name);
            if ((r64(r + 8) >> 32) >= o->nsym) fail("%s: relocation %u in '%s' uses a bad symbol index", o->name, k, t->name);
        }
    }
}

/* ---------- разрешение глобальных символов ---------- */
static void resolve_symbols(void) {
    size_t tot = 0;
    for (unsigned i = 0; i < C.nobjs; i++) tot += C.objs[i].nsym;
    size_t sz = 64;
    while (sz < tot * 2 + 64) sz <<= 1;
    C.htsize = sz;
    C.ht = anew(sizeof(GSym *) * sz);
    for (unsigned oi = 0; oi < C.nobjs; oi++) {
        KObj *o = &C.objs[oi];
        for (uint32_t i = 1; i < o->nsym; i++) {
            KSym *y = &o->sym[i];
            unsigned bind = y->info >> 4;
            if (bind == STB_LOCAL) continue;
            if (!y->name[0]) fail("%s: global symbol %u has no name", o->name, i);
            GSym *g = gget(y->name);
            y->g = g;
            if ((y->other & 3) == 1 || (y->other & 3) == 2) g->hidden = 1;
            if (y->shndx == SHN_UNDEF) {
                if (bind == STB_WEAK) g->wref = 1;
                continue;
            }
            int weak = bind == STB_WEAK;
            int common = y->shndx == SHN_COMMON;
            if (g->state == GS_UNDEF) {
                g->state = common ? GS_COMMON : GS_DEF;
            } else if (g->state == GS_COMMON) {
                if (common) {
                    if (y->size > g->size) g->size = y->size;
                    if (y->value > g->calign) g->calign = y->value;
                    continue;
                }
                g->state = GS_DEF;
            } else if (common) {
                continue;
            } else if (g->weak && !weak) {
                g->state = GS_DEF;
            } else if (!g->weak && !weak) {
                fail("duplicate symbol '%s': defined in %s and in %s", g->name, g->obj->name, o->name);
            } else {
                continue;
            }
            g->weak = (uint8_t)weak;
            g->obj = o;
            g->type = y->info & 15;
            g->size = y->size;
            if (common) {
                g->calign = y->value;
            } else if (y->shndx == SHN_ABS) {
                g->sec = NULL;
                g->value = y->value;
            } else {
                g->sec = &o->sec[y->shndx];
                g->value = y->value;
            }
        }
    }
    /* общие символы (COMMON): одна псевдосекция, по убыванию выравнивания, как у ld.lld */
    unsigned nc = 0;
    for (GSym *g = C.gfirst; g; g = g->lnext)
        if (g->state == GS_COMMON) nc++;
    if (!nc) return;
    GSym **v = anew(sizeof(GSym *) * nc);
    unsigned k = 0;
    for (GSym *g = C.gfirst; g; g = g->lnext)
        if (g->state == GS_COMMON) v[k++] = g;
    for (unsigned i = 1; i < nc; i++) {                  /* устойчивая сортировка вставками */
        GSym *x = v[i];
        unsigned j = i;
        while (j > 0 && v[j - 1]->calign < x->calign) { v[j] = v[j - 1]; j--; }
        v[j] = x;
    }
    KObj *po = anew(sizeof *po);
    po->name = "(common)";
    KSec *cs = anew(sizeof *cs);
    cs->obj = po;
    cs->name = "COMMON";
    cs->type = SHT_NOBITS;
    cs->flags = SHF_ALLOC | SHF_WRITE;
    cs->cand = 1;
    cs->align = 1;
    uint64_t off = 0;
    for (unsigned i = 0; i < nc; i++) {
        if (v[i]->calign > cs->align) cs->align = v[i]->calign;
        off = alup(off, v[i]->calign);
        v[i]->sec = cs;
        v[i]->value = off;
        v[i]->state = GS_DEF;
        v[i]->obj = po;
        off += v[i]->size;
        if (off > MAX_SEC_SIZE) fail("common symbols are too large");
    }
    cs->size = off;
    C.commonsec = cs;
}

/* ---------- лексер и разбор скрипта ---------- */
static int is_wordc(int c, int first) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '$' || c == '*' || c == '?') return 1;
    return !first && c >= '0' && c <= '9';
}

static int ev_depth;

static void lex_script(const char *s, size_t n) {
    ev_depth = 0;
    C.tok = anew(sizeof(Tok) * (n + 2));
    int line = 1;
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\n') { line++; i++; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '/')) { if (s[j] == '\n') line++; j++; }
            if (j + 1 >= n) fail("script:%d: unterminated comment", line);
            i = j + 2;
            continue;
        }
        Tok *t = &C.tok[C.ntok++];
        t->line = line;
        t->s = s + i;
        if (c == '/' && n - i >= 9 && memcmp(s + i, "/DISCARD/", 9) == 0) {
            t->type = T_WORD; t->len = 9; i += 9;
        } else if (c >= '0' && c <= '9') {
            uint64_t v = 0;
            size_t j = i;
            if (c == '0' && j + 1 < n && (s[j + 1] == 'x' || s[j + 1] == 'X')) {
                j += 2;
                size_t st = j;
                while (j < n && ((s[j] >= '0' && s[j] <= '9') || (s[j] >= 'a' && s[j] <= 'f') || (s[j] >= 'A' && s[j] <= 'F'))) {
                    if (v >> 60) fail("script:%d: number is too large", line);
                    int dg = s[j] <= '9' ? s[j] - '0' : (s[j] | 32) - 'a' + 10;
                    v = v * 16 + (uint64_t)dg;
                    j++;
                }
                if (j == st) fail("script:%d: bad hexadecimal number", line);
            } else {
                while (j < n && s[j] >= '0' && s[j] <= '9') {
                    if (v > (UINT64_MAX - 9) / 10) fail("script:%d: number is too large", line);
                    v = v * 10 + (uint64_t)(s[j] - '0');
                    j++;
                }
            }
            if (j < n && (s[j] == 'K' || s[j] == 'k')) { if (v >> 54) fail("script:%d: number is too large", line); v <<= 10; j++; }
            else if (j < n && (s[j] == 'M' || s[j] == 'm')) { if (v >> 44) fail("script:%d: number is too large", line); v <<= 20; j++; }
            if (j < n && is_wordc((unsigned char)s[j], 0)) fail("script:%d: bad number", line);
            t->type = T_NUM; t->num = v; t->len = (int)(j - i); i = j;
        } else if (is_wordc((unsigned char)c, 1)) {
            size_t j = i + 1;
            while (j < n && is_wordc((unsigned char)s[j], 0)) j++;
            t->type = T_WORD; t->len = (int)(j - i); i = j;
        } else if (c && strchr("{}():;=+-/&|~!<>,%", c)) {
            t->type = T_PUNCT; t->len = 1;
            if (i + 1 < n && ((c == '+' && s[i + 1] == '=') || (c == '-' && s[i + 1] == '=') || (c == '<' && s[i + 1] == '<') || (c == '>' && s[i + 1] == '>'))) t->len = 2;
            i += (size_t)t->len;
        } else {
            fail("script:%d: unsupported character '%c'", line, c >= 32 && c < 127 ? c : '?');
        }
    }
    C.tok[C.ntok].type = T_END;
    C.tok[C.ntok].line = line;
}

static int ti;                                           /* текущий токен при разборе */
static Tok *cur(void) { return &C.tok[ti]; }
static int is_p(const Tok *t, const char *p) { return t->type == T_PUNCT && (size_t)t->len == strlen(p) && memcmp(t->s, p, (size_t)t->len) == 0; }
static int is_w(const Tok *t, const char *w) { return t->type == T_WORD && (size_t)t->len == strlen(w) && memcmp(t->s, w, (size_t)t->len) == 0; }
static const char *tok_text(const Tok *t) { return t->type == T_END ? "end of file" : t->type == T_NUM ? "number" : "token"; }
static void unsupported(const char *what) {
    Tok *t = cur();
    if (t->type == T_WORD) fail("script:%d: unsupported %s '%.*s'", t->line, what, t->len, t->s);
    fail("script:%d: unsupported %s (%s)", t->line, what, tok_text(t));
}
static void expect_p(const char *p) {
    if (!is_p(cur(), p)) fail("script:%d: expected '%s'", cur()->line, p);
    ti++;
}
static char *dup_word(const Tok *t) {
    char *p = anew((size_t)t->len + 1);
    memcpy(p, t->s, (size_t)t->len);
    return p;
}
static char *take_word(void) {
    if (cur()->type != T_WORD) fail("script:%d: expected a name", cur()->line);
    return dup_word(&C.tok[ti++]);
}

/* ---- выражения: вычисляются по токенам на месте ---- */
static uint64_t ev_or(uint64_t dot, int run);
static GSym *script_sym(const char *name) {
    GSym *g = gfind(name);
    return g && g->state == GS_SCRIPT ? g : NULL;
}

static uint64_t ev_primary1(uint64_t dot, int run);
static uint64_t ev_primary(uint64_t dot, int run) {
    if (++ev_depth > 64) fail("script:%d: expression is nested too deeply", cur()->line);
    uint64_t v = ev_primary1(dot, run);
    ev_depth--;
    return v;
}
static uint64_t ev_primary1(uint64_t dot, int run) {
    Tok *t = cur();
    if (t->type == T_NUM) { ti++; return t->num; }
    if (is_p(t, "(")) { ti++; uint64_t v = ev_or(dot, run); expect_p(")"); return v; }
    if (is_p(t, "-")) { ti++; return (uint64_t)0 - ev_primary(dot, run); }
    if (is_p(t, "~")) { ti++; return ~ev_primary(dot, run); }
    if (is_p(t, "!")) { ti++; return ev_primary(dot, run) == 0; }
    if (t->type != T_WORD) fail("script:%d: expected an expression", t->line);
    if (t->len == 1 && t->s[0] == '.') { ti++; return dot; }
    if (is_w(t, "ALIGN")) {
        ti++;
        expect_p("(");
        uint64_t a = ev_or(dot, run), v = dot;
        if (is_p(cur(), ",")) { ti++; v = a; a = ev_or(dot, run); }
        expect_p(")");
        if (run && (a == 0 || (a & (a - 1)) || a > MAX_ALIGN)) fail("script:%d: ALIGN() argument must be a power of two", t->line);
        return run ? alup(v, a) : 0;
    }
    if (ti + 1 < C.ntok && is_p(&C.tok[ti + 1], "(")) fail("script:%d: unsupported function '%.*s'", t->line, t->len, t->s);
    ti++;
    if (!run) return 0;
    char *nm = dup_word(t);
    GSym *g = script_sym(nm);
    if (!g) fail("script:%d: symbol '%s' used in an expression is not defined earlier in the script", t->line, nm);
    return g->value;
}
static uint64_t ev_mul(uint64_t dot, int run) {
    uint64_t v = ev_primary(dot, run);
    for (;;) {
        Tok *t = cur();
        if (is_w(t, "*")) { ti++; v *= ev_primary(dot, run); }
        else if (is_p(t, "/") || is_p(t, "%")) {
            ti++;
            uint64_t d = ev_primary(dot, run);
            if (run && d == 0) fail("script:%d: division by zero", t->line);
            if (run) v = t->s[0] == '/' ? v / d : v % d;
        } else return v;
    }
}
static uint64_t ev_add(uint64_t dot, int run) {
    uint64_t v = ev_mul(dot, run);
    for (;;) {
        if (is_p(cur(), "+")) { ti++; v += ev_mul(dot, run); }
        else if (is_p(cur(), "-")) { ti++; v -= ev_mul(dot, run); }
        else return v;
    }
}
static uint64_t ev_shift(uint64_t dot, int run) {
    uint64_t v = ev_add(dot, run);
    for (;;) {
        if (is_p(cur(), "<<")) { ti++; uint64_t s = ev_add(dot, run); v = s < 64 ? v << s : 0; }
        else if (is_p(cur(), ">>")) { ti++; uint64_t s = ev_add(dot, run); v = s < 64 ? v >> s : 0; }
        else return v;
    }
}
static uint64_t ev_and(uint64_t dot, int run) {
    uint64_t v = ev_shift(dot, run);
    while (is_p(cur(), "&")) { ti++; v &= ev_shift(dot, run); }
    return v;
}
static uint64_t ev_or(uint64_t dot, int run) {
    uint64_t v = ev_and(dot, run);
    while (is_p(cur(), "|")) { ti++; v |= ev_and(dot, run); }
    return v;
}
static uint64_t eval_at(int tok, uint64_t dot) {
    int save = ti;
    ti = tok;
    uint64_t v = ev_or(dot, 1);
    ti = save;
    return v;
}
static void skip_expr(void) { (void)ev_or(0, 0); }

/* ---- разбор команд ---- */
static SStmt *new_stmt(int kind) {
    SStmt *s = anew(sizeof *s);
    s->kind = kind;
    s->line = cur()->line;
    s->addr_tok = -1;
    s->align_tok = -1;
    return s;
}

static SStmt *parse_assign(int provide) {
    SStmt *s = new_stmt(S_ASSIGN);
    s->provide = provide;
    s->name = take_word();
    if (is_p(cur(), "=")) { s->op = 0; ti++; }
    else if (is_p(cur(), "+=")) { s->op = 1; ti++; }
    else unsupported("assignment operator");
    s->tok = ti;
    skip_expr();
    return s;
}

static SStmt *parse_provide(void) {
    ti++;
    expect_p("(");
    SStmt *s = parse_assign(1);
    expect_p(")");
    expect_p(";");
    return s;
}

static SStmt *parse_input(int keep) {
    SStmt *s = new_stmt(S_INPUT);
    s->keep = keep;
    const Tok *ft = cur();
    if (ft->type != T_WORD) fail("script:%d: expected a file pattern", ft->line);
    s->file = take_word();
    expect_p("(");
    int start = ti, n = 0;
    while (!is_p(&C.tok[ti], ")")) {
        if (C.tok[ti].type != T_WORD) fail("script:%d: expected a section pattern", C.tok[ti].line);
        if (strncmp(C.tok[ti].s, "SORT", 4) == 0 || strncmp(C.tok[ti].s, "EXCLUDE_FILE", 12) == 0)
            fail("script:%d: unsupported pattern modifier '%.*s'", C.tok[ti].line, C.tok[ti].len, C.tok[ti].s);
        n++; ti++;
    }
    if (n == 0) fail("script:%d: empty section pattern list", ft->line);
    s->pat = anew(sizeof(char *) * (size_t)n);
    for (int i = 0; i < n; i++) s->pat[i] = dup_word(&C.tok[start + i]);
    s->npat = n;
    expect_p(")");
    return s;
}

static SStmt *parse_body(void) {
    SStmt *head = NULL, *tail = NULL;
    while (!is_p(cur(), "}")) {
        SStmt *s;
        Tok *t = cur();
        if (t->type == T_END) fail("script:%d: unexpected end of file inside a section", t->line);
        if (t->type != T_WORD) unsupported("statement");
        if (is_w(t, "PROVIDE") || is_w(t, "PROVIDE_HIDDEN")) {
            s = parse_provide();
        } else if (is_w(t, "KEEP")) {
            ti++;
            expect_p("(");
            s = parse_input(1);
            expect_p(")");
        } else if (ti + 1 < C.ntok && (is_p(&C.tok[ti + 1], "=") || is_p(&C.tok[ti + 1], "+="))) {
            s = parse_assign(0);
            expect_p(";");
        } else if (ti + 1 < C.ntok && is_p(&C.tok[ti + 1], "(")) {
            s = parse_input(0);
        } else {
            unsupported("statement");
            return NULL;
        }
        if (is_p(cur(), ";") || is_p(cur(), ",")) ti++;
        if (tail) tail->next = s; else head = s;
        tail = s;
    }
    return head;
}

static void parse_phdrs(void) {
    expect_p("{");
    while (!is_p(cur(), "}")) {
        if (C.nph >= MAX_PHDRS) fail("script:%d: too many program headers", cur()->line);
        PhDef *p = &C.ph[C.nph++];
        p->name = take_word();
        const Tok *tt = cur();
        if (!is_w(tt, "PT_LOAD")) fail("script:%d: unsupported program header type (only PT_LOAD)", tt->line);
        ti++;
        if (is_w(cur(), "FLAGS")) {
            ti++;
            expect_p("(");
            uint64_t f = ev_or(0, 1);
            expect_p(")");
            if (f > 7) fail("script:%d: bad segment FLAGS", tt->line);
            p->has_flags = 1;
            p->flags = (uint32_t)f;
        }
        if (is_w(cur(), "FILEHDR") || is_w(cur(), "PHDRS") || is_w(cur(), "AT")) unsupported("program header attribute");
        expect_p(";");
    }
    ti++;
    C.have_phdrs = 1;
}

static void parse_sections(void) {
    expect_p("{");
    SStmt *tail = NULL;
    while (!is_p(cur(), "}")) {
        SStmt *s;
        Tok *t = cur();
        if (t->type == T_END) fail("script:%d: unexpected end of file inside SECTIONS", t->line);
        if (t->type != T_WORD) unsupported("statement");
        if (is_w(t, "PROVIDE") || is_w(t, "PROVIDE_HIDDEN")) {
            s = parse_provide();
        } else if (ti + 1 < C.ntok && (is_p(&C.tok[ti + 1], "=") || is_p(&C.tok[ti + 1], "+="))) {
            s = parse_assign(0);
            expect_p(";");
        } else {
            s = new_stmt(S_OUT);
            s->name = take_word();
            s->discard = strcmp(s->name, "/DISCARD/") == 0;
            if (!is_p(cur(), ":")) {
                s->addr_tok = ti;
                skip_expr();
            }
            expect_p(":");
            if (is_w(cur(), "ALIGN")) {
                ti++;
                expect_p("(");
                s->align_tok = ti;
                skip_expr();
                expect_p(")");
            }
            if (is_w(cur(), "AT") || is_w(cur(), "SUBALIGN") || is_w(cur(), "ONLY_IF_RO") || is_w(cur(), "NOLOAD")) unsupported("output section attribute");
            expect_p("{");
            s->body = parse_body();
            expect_p("}");
            for (;;) {
                if (is_p(cur(), ":")) {
                    ti++;
                    if (s->nph >= 4) fail("script:%d: too many program headers for one section", cur()->line);
                    s->phn[s->nph++] = take_word();
                } else if (is_p(cur(), ">") || is_p(cur(), "=")) {
                    unsupported("output section attribute (memory region or fill)");
                } else break;
            }
            if (is_p(cur(), ";")) ti++;
        }
        if (tail) tail->next = s; else C.items = s;
        tail = s;
    }
    ti++;
    C.have_sections = 1;
}

static void parse_script(const char *s, size_t n) {
    lex_script(s, n);
    ti = 0;
    while (cur()->type != T_END) {
        Tok *t = cur();
        if (is_w(t, "ENTRY")) {
            ti++;
            expect_p("(");
            C.entry = take_word();
            expect_p(")");
        } else if (is_w(t, "PHDRS")) {
            ti++;
            parse_phdrs();
        } else if (is_w(t, "SECTIONS")) {
            if (C.have_sections) fail("script:%d: more than one SECTIONS command", t->line);
            ti++;
            parse_sections();
        } else {
            unsupported("command");
        }
        if (is_p(cur(), ";")) ti++;
    }
    if (!C.have_sections) fail("script has no SECTIONS command");
}

/* ---------- привязка входных секций ---------- */
static int glob(const char *p, const char *s) {
    for (; *p; p++) {
        if (*p == '*') {
            while (p[1] == '*') p++;
            if (!p[1]) return 1;
            for (; *s; s++)
                if (glob(p + 1, s)) return 1;
            return glob(p + 1, s);
        }
        if (!*s) return 0;
        if (*p != '?' && *p != *s) return 0;
        s++;
    }
    return *s == 0;
}

static int stmt_matches(const SStmt *st, const KSec *s) {
    const char *fn = s->obj->name;
    if (!glob(st->file, fn) && !glob(st->file, base_name(fn))) return 0;
    for (int i = 0; i < st->npat; i++)
        if (glob(st->pat[i], s->name)) return 1;
    return 0;
}

static void bind_sections(void) {
    unsigned nsec = 0;
    for (unsigned i = 0; i < C.nobjs; i++) nsec += C.objs[i].nsec;
    KSec **all = anew(sizeof(KSec *) * (nsec + 2));
    unsigned na = 0;
    for (unsigned oi = 0; oi < C.nobjs; oi++)
        for (unsigned i = 1; i < C.objs[oi].nsec; i++) all[na++] = &C.objs[oi].sec[i];
    if (C.commonsec) all[na++] = C.commonsec;
    for (unsigned a = 0; a < na; a++) {
        KSec *s = all[a];
        if (s->type == SHT_NULL || s->type == SHT_SYMTAB || s->type == SHT_STRTAB || s->type == SHT_RELA) continue;
        int found = 0;
        for (SStmt *o = C.items; o && !found; o = o->next) {
            if (o->kind != S_OUT) continue;
            for (SStmt *st = o->body; st; st = st->next) {
                if (st->kind != S_INPUT || !stmt_matches(st, s)) continue;
                found = 1;
                s->stmt = st;
                s->discard = (uint8_t)o->discard;
                if (!o->discard) {
                    if (st->tail) st->tail->snext = s; else st->head = s;
                    st->tail = s;
                }
                break;
            }
        }
        if (!(s->flags & SHF_ALLOC)) {
            if (found && !s->discard) fail("%s: non-allocatable section '%s' is placed in an output section by the script (unsupported)", s->obj->name, s->name);
            if (!found && strcmp(s->name, ".comment") == 0 && s->size) {
                if (!C.comment_first) C.comment_first = s;
            }
            if (!found && s->type != SHT_LLVM_ADDRSIG && strncmp(s->name, ".note", 5) != 0 && strncmp(s->name, ".debug", 6) != 0 &&
                strcmp(s->name, ".comment") != 0 && s->size)
                fail("%s: non-allocatable section '%s' is not handled by the script (unsupported)", s->obj->name, s->name);
            continue;
        }
        if (!found) {
            if (s->size == 0) { s->discard = 1; continue; }
            fail("%s: allocatable section '%s' is not placed by the linker script (add a rule or /DISCARD/)", s->obj->name, s->name);
        }
        if (!s->discard) C.nsec_in++;
    }
}

/* ---------- --gc-sections ---------- */
static KSec *sym_section(KObj *o, uint32_t si) {
    KSym *y = &o->sym[si];
    if (y->g) return y->g->state == GS_DEF ? y->g->sec : NULL;
    if (y->shndx == SHN_UNDEF || y->shndx >= 0xff00) return NULL;
    return &o->sec[y->shndx];
}

static void mark_gc(void) {
    unsigned nsec = 1;
    for (unsigned i = 0; i < C.nobjs; i++) nsec += C.objs[i].nsec;
    KSec **stk = anew(sizeof(KSec *) * (nsec + 1));
    unsigned sp = 0;
    for (SStmt *o = C.items; o; o = o->next) {
        if (o->kind != S_OUT || o->discard) continue;
        for (SStmt *st = o->body; st; st = st->next)
            if (st->kind == S_INPUT && st->keep)
                for (KSec *s = st->head; s; s = s->snext)
                    if (!s->live) { s->live = 1; stk[sp++] = s; }
    }
    if (C.entry) {
        GSym *g = gfind(C.entry);
        if (g && g->state == GS_DEF && g->sec && g->sec->stmt && !g->sec->discard && !g->sec->live) { g->sec->live = 1; stk[sp++] = g->sec; }
    }
    while (sp) {
        KSec *s = stk[--sp];
        for (uint32_t k = 0; k < s->nrela; k++) {
            uint32_t si = (uint32_t)(r64(s->rela + (uint64_t)k * 24 + 8) >> 32);
            KSec *t = sym_section(s->obj, si);
            if (t && t->cand && t->stmt && !t->discard && !t->live) { t->live = 1; stk[sp++] = t; }
        }
    }
    for (unsigned oi = 0; oi < C.nobjs; oi++)
        for (unsigned i = 1; i < C.objs[oi].nsec; i++) {
            KSec *s = &C.objs[oi].sec[i];
            if (s->cand && s->stmt && !s->discard && !s->live) C.nsec_gc++;
        }
}

/* ---------- раскладка ---------- */
static GSym *define_script_sym(SStmt *s, uint64_t v, KOut *owner, int inside) {
    GSym *g = gfind(s->name);
    if (s->provide) {
        if (!g || g->state != GS_UNDEF) return NULL;      /* PROVIDE: только если на символ есть ссылка и его никто не определил */
    } else if (g && g->state != GS_UNDEF) {
        if (g->state == GS_SCRIPT) { g->value = v; g->owner = owner; g->inside = (uint8_t)inside; return g; }
        fail("script:%d: symbol '%s' is defined by the script and by %s", s->line, s->name, g->obj ? g->obj->name : "an object");
    }
    if (!g) g = gget(s->name);
    g->state = GS_SCRIPT;
    g->value = v;
    g->owner = owner;
    g->inside = (uint8_t)inside;
    return g;
}

static void check_dot(uint64_t d, int line) {
    if (d > MAX_ADDR) fail("script:%d: address 0x%llx is out of range", line, (unsigned long long)d);
}

static int lay_assign(SStmt *s, uint64_t *dot, KOut *lastout, KOut *owner, int inside, int *dot_set) {
    uint64_t v = eval_at(s->tok, *dot);
    if (s->name[0] == '.' && s->name[1] == 0) {
        if (s->op == 1) v += *dot;
        if (v < *dot && *dot_set) fail("script:%d: the location counter cannot move backwards (0x%llx -> 0x%llx)", s->line, (unsigned long long)*dot, (unsigned long long)v);
        check_dot(v, s->line);
        *dot = v;
        *dot_set = 1;
        return 1;
    }
    if (s->op == 1) fail("script:%d: '+=' is only supported for the location counter", s->line);
    (void)lastout;
    define_script_sym(s, v, owner ? owner : lastout, inside);
    return 0;
}

static void layout(void) {
    uint64_t dot = 0;
    int dot_set = 0;
    KOut *last = NULL;
    for (SStmt *it = C.items; it; it = it->next) {
        if (it->kind == S_ASSIGN) {
            lay_assign(it, &dot, last, NULL, 0, &dot_set);
            continue;
        }
        if (it->discard) continue;
        KOut *o = anew(sizeof *o);
        o->name = it->name;
        o->st = it;
        it->out = o;
        o->seg = -1;
        o->align = 1;
        int nmem = 0, nb = 0;
        for (SStmt *st = it->body; st; st = st->next) {
            if (st->kind != S_INPUT) continue;
            for (KSec *s = st->head; s; s = s->snext) {
                if (!s->live) continue;
                nmem++;
                if (s->type == SHT_NOBITS) nb++;
                if (s->align > o->align) o->align = s->align;
                if (s->flags & SHF_WRITE) o->flags |= SHF_WRITE;
                if (s->flags & SHF_EXEC) o->flags |= SHF_EXEC;
            }
        }
        o->flags |= SHF_ALLOC;
        o->nobits = nmem > 0 && nb == nmem;
        uint64_t save = dot;
        if (it->addr_tok >= 0) { dot = eval_at(it->addr_tok, dot); check_dot(dot, it->line); dot_set = 1; }
        if (!dot_set) fail("script:%d: SECTIONS must set the start address ('. = ...') before the first output section", it->line);
        if (it->align_tok >= 0) {
            uint64_t a = eval_at(it->align_tok, dot);
            if (a == 0 || (a & (a - 1)) || a > MAX_ALIGN) fail("script:%d: section alignment must be a power of two", it->line);
            if (a > o->align) o->align = a;
        }
        dot = alup(dot, o->align);
        check_dot(dot, it->line);
        o->va = dot;
        for (SStmt *st = it->body; st; st = st->next) {
            if (st->kind == S_ASSIGN) {
                lay_assign(st, &dot, last, o, 1, &dot_set);
            } else {
                for (KSec *s = st->head; s; s = s->snext) {
                    if (!s->live) continue;
                    dot = alup(dot, s->align);
                    s->out = o;
                    s->addr = dot;
                    s->off = dot - o->va;
                    dot += s->size;
                    check_dot(dot, it->line);
                }
            }
        }
        o->size = dot - o->va;
        o->nonempty = nmem > 0 || o->size > 0;
        if (!o->nonempty) {
            if (it->addr_tok < 0) dot = save;
            continue;
        }
        if (o->size > MAX_IMAGE * 64) fail("section '%s' is too large", o->name);
        if (C.outs_tail) C.outs_tail->next = o; else C.outs = o;
        C.outs_tail = o;
        last = o;
    }
}

/* ---------- программные заголовки и смещения в файле ---------- */
static uint32_t out_perm(const KOut *o) { return 4 | ((o->flags & SHF_WRITE) ? 2 : 0) | ((o->flags & SHF_EXEC) ? 1 : 0); }

static int ph_has(const KOut *o, const char *name) {
    for (int q = 0; o->phst && q < o->phst->nph; q++)
        if (strcmp(o->phst->phn[q], name) == 0) return 1;
    return 0;
}

static int build_segments(Seg *seg) {
    int ns = 0;
    if (C.have_phdrs) {
        SStmt *prev = NULL;
        for (KOut *o = C.outs; o; o = o->next) {
            o->phst = o->st->nph ? o->st : prev;
            prev = o->phst;
            for (int q = 0; o->phst && q < o->phst->nph; q++) {
                int known = 0;
                for (int p = 0; p < C.nph; p++)
                    if (strcmp(o->phst->phn[q], C.ph[p].name) == 0) known = 1;
                if (!known) fail("script:%d: unknown program header '%s'", o->phst->line, o->phst->phn[q]);
            }
        }
        for (int p = 0; p < C.nph; p++) {
            Seg *s = &seg[ns];
            int idx = 0, first = -1, lastidx = -1;
            for (KOut *o = C.outs; o; o = o->next, idx++) {
                if (!ph_has(o, C.ph[p].name)) continue;
                if (first < 0) first = idx;
                lastidx = idx;
            }
            if (first < 0) continue;
            idx = 0;
            for (KOut *o = C.outs; o; o = o->next, idx++) {
                int in = ph_has(o, C.ph[p].name);
                if (idx >= first && idx <= lastidx && !in) fail("program header '%s' is not contiguous (section '%s' lies inside it)", C.ph[p].name, o->name);
                if (!in) continue;
                if (o->seg >= 0) fail("section '%s' belongs to several program headers (unsupported)", o->name);
                o->seg = ns;
                if (!s->first) s->first = o;
                s->last = o;
            }
            s->flags = 0;
            for (KOut *o = s->first;; o = o->next) {
                s->flags |= out_perm(o);
                if (o == s->last) break;
            }
            if (C.ph[p].has_flags) s->flags = C.ph[p].flags;
            ns++;
        }
        /* GNU ld: секция без :phdr входит в сегменты предыдущей; здесь это проверено выше как принадлежность, остальное - ошибка */
        for (KOut *o = C.outs; o; o = o->next)
            if (o->seg < 0) fail("section '%s' is not assigned to any program header", o->name);
    } else {
        Seg *s = NULL;
        for (KOut *o = C.outs; o; o = o->next) {
            if (!s || out_perm(o) != s->flags || (s->last->nobits && !o->nobits)) {
                if (ns >= MAX_PHDRS) fail("too many segments");
                s = &seg[ns++];
                s->flags = out_perm(o);
                s->first = o;
            }
            s->last = o;
            o->seg = ns - 1;
        }
    }
    for (int i = 0; i < ns; i++) {
        Seg *s = &seg[i];
        s->va = s->first->va;
        s->memsz = s->last->va + s->last->size - s->va;
        uint64_t fend = s->va;
        for (KOut *o = s->first;; o = o->next) {
            if (!o->nobits) fend = o->va + o->size;
            if (o == s->last) break;
        }
        s->filesz = fend - s->va;
    }
    return ns;
}

/* ---------- перемещения ---------- */
static const char *sym_label(KObj *o, uint32_t si) {
    KSym *y = &o->sym[si];
    if (y->name[0]) return y->name;
    if ((y->info & 15) == STT_SECTION && y->shndx < o->nsec) return o->sec[y->shndx].name;
    return "(unnamed)";
}

static uint64_t sym_value(KObj *o, KSec *from, uint64_t roff, uint32_t si, uint32_t type, int *defined) {
    KSym *y = &o->sym[si];
    *defined = 1;
    if (y->g) {
        GSym *g = y->g;
        if (g->state == GS_UNDEF) {
            if ((y->info >> 4) == STB_WEAK) { *defined = 0; return 0; }
            fail("%s:(%s+0x%llx): undefined reference to '%s'", o->name, from->name, (unsigned long long)roff, g->name);
        }
        if (g->state == GS_SCRIPT) return g->value;
        if (!g->sec) return g->value;
        if (!g->sec->out) {
            fail("%s:(%s+0x%llx): relocation %s against '%s' which is defined in discarded section '%s' of %s", o->name, from->name, (unsigned long long)roff,
                 typename_rel(type), g->name, g->sec->name, g->sec->obj->name);
        }
        return g->sec->addr + g->value;
    }
    if (y->shndx == SHN_ABS) return y->value;
    if (y->shndx == SHN_UNDEF) fail("%s:(%s+0x%llx): relocation against symbol 0", o->name, from->name, (unsigned long long)roff);
    if (y->shndx >= 0xff00) fail("%s: local symbol '%s' has an unsupported section index", o->name, sym_label(o, si));
    KSec *t = &o->sec[y->shndx];
    if (!t->out) {
        fail("%s:(%s+0x%llx): relocation %s against '%s' in discarded section '%s'", o->name, from->name, (unsigned long long)roff, typename_rel(type),
             sym_label(o, si), t->name);
    }
    return t->addr + y->value;
}

static void reloc_section(KSec *s, uint8_t *img) {
    KObj *o = s->obj;
    uint8_t *dst = img + s->out->off + s->off;
    for (uint32_t k = 0; k < s->nrela; k++) {
        const uint8_t *r = s->rela + (uint64_t)k * 24;
        uint64_t off = r64(r), info = r64(r + 8);
        int64_t add = (int64_t)r64(r + 16);
        uint32_t type = (uint32_t)info, si = (uint32_t)(info >> 32);
        uint64_t P = s->addr + off;
        int width = type == R_64 || type == R_PC64 ? 8 : 4;
        if (type == R_NONE) continue;
        if (type != R_64 && type != R_PC32 && type != R_PLT32 && type != R_32 && type != R_32S && type != R_PC64 && type != R_GOTPCREL &&
            type != R_GOTPCRELX && type != R_REX_GOTPCRELX) {
            fail("%s:(%s+0x%llx): unsupported relocation type %u (%s) against '%s'", o->name, s->name, (unsigned long long)off, type, typename_rel(type),
                 sym_label(o, si));
        }
        if (s->size - off < (uint64_t)width) fail("%s:(%s+0x%llx): relocation does not fit into the section", o->name, s->name, (unsigned long long)off);
        int defined;
        uint64_t S = sym_value(o, s, off, si, type, &defined);
        if (type == R_GOTPCREL || type == R_GOTPCRELX || type == R_REX_GOTPCRELX) {
            /* GOT нет: допустима только релаксация `mov sym@GOTPCREL(%rip), reg` в `lea sym(%rip), reg` для определённого символа */
            if (type == R_GOTPCREL || !defined || off < 2 || dst[off - 2] != 0x8b || (dst[off - 1] & 0xc7) != 0x05)
                fail("%s:(%s+0x%llx): %s against '%s' needs a GOT, which is not supported (only `mov sym@GOTPCREL(%%rip), reg` of a defined symbol is relaxed)", o->name,
                     s->name, (unsigned long long)off, typename_rel(type), sym_label(o, si));
            dst[off - 2] = 0x8d;
            type = R_PC32;
        }
        uint64_t v = S + (uint64_t)add;
        switch (type) {
        case R_64:
            w64(dst + off, v);
            break;
        case R_PC64:
            w64(dst + off, v - P);
            break;
        case R_PC32: case R_PLT32: {
            int64_t d = (int64_t)(v - P);
            if (d < INT32_MIN || d > INT32_MAX)
                fail("%s:(%s+0x%llx): relocation %s overflows against '%s' (distance 0x%llx does not fit in 32 bits)", o->name, s->name, (unsigned long long)off,
                     type == R_PC32 ? "R_X86_64_PC32" : "R_X86_64_PLT32", sym_label(o, si), (unsigned long long)d);
            w32(dst + off, (uint32_t)d);
            break;
        }
        case R_32:
            if (v > 0xffffffffULL)
                fail("%s:(%s+0x%llx): relocation R_X86_64_32 overflows against '%s' (value 0x%llx does not fit in 32 bits)", o->name, s->name,
                     (unsigned long long)off, sym_label(o, si), (unsigned long long)v);
            w32(dst + off, (uint32_t)v);
            break;
        case R_32S:
            if ((int64_t)v < INT32_MIN || (int64_t)v > INT32_MAX)
                fail("%s:(%s+0x%llx): relocation R_X86_64_32S overflows against '%s' (value 0x%llx does not fit in signed 32 bits)", o->name, s->name,
                     (unsigned long long)off, sym_label(o, si), (unsigned long long)v);
            w32(dst + off, (uint32_t)v);
            break;
        default:
            fail("internal: relocation type %u", type);
        }
    }
}

/* ---------- запись ELF ---------- */
typedef struct { uint8_t *p; size_t len; } Buf;

static size_t put_sym(uint8_t *tab, size_t *nsym, uint8_t *str, size_t *slen, const char *name, uint8_t info, uint16_t shndx, uint64_t val, uint64_t size) {
    size_t nl = strlen(name) + 1, at = *slen;
    if (str) { memcpy(str + at, name, nl); }
    *slen += nl;
    if (tab) {
        uint8_t *e = tab + *nsym * 24;
        w32(e, name[0] ? (uint32_t)at : 0);
        e[4] = info;
        w16(e + 6, shndx);
        w64(e + 8, val);
        w64(e + 16, size);
    }
    return (*nsym)++;
}

/* Обход таблицы символов выхода; tab == NULL - только подсчёт. Возвращает число символов, в *first_global - индекс первого глобального. */
static size_t emit_symbols(uint8_t *tab, uint8_t *str, size_t *slen, size_t *first_global) {
    size_t n = 0;
    *slen = 1;
    put_sym(tab, &n, str, slen, "", 0, 0, 0, 0);
    for (unsigned oi = 0; oi < C.nobjs; oi++) {
        KObj *o = &C.objs[oi];
        const char *fname = NULL;
        for (uint32_t i = 1; i < o->nsym && !fname; i++)
            if ((o->sym[i].info & 15) == STT_FILE && o->sym[i].name[0]) fname = o->sym[i].name;
        if (fname) put_sym(tab, &n, str, slen, fname, (uint8_t)((STB_LOCAL << 4) | STT_FILE), SHN_ABS, 0, 0);
        for (uint32_t i = 1; i < o->nsym; i++) {
            KSym *y = &o->sym[i];
            unsigned type = y->info & 15;
            if ((y->info >> 4) != STB_LOCAL || type == STT_SECTION || type == STT_FILE || !y->name[0]) continue;
            uint64_t val;
            uint16_t sh;
            if (y->shndx == SHN_ABS) { val = y->value; sh = SHN_ABS; }
            else if (y->shndx >= 0xff00 || y->shndx == 0) continue;
            else {
                KSec *t = &o->sec[y->shndx];
                if (!t->out || !t->live) continue;
                if (y->name[0] == '.' && y->name[1] == 'L' && (t->flags & SHF_MERGE)) continue;   /* как у ld.lld: временные метки в сливаемых секциях */
                val = t->addr + y->value;
                sh = (uint16_t)t->out->shndx;
            }
            put_sym(tab, &n, str, slen, y->name, (uint8_t)((STB_LOCAL << 4) | type), sh, val, y->size);
        }
    }
    KOut *firstout = C.outs;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) *first_global = n;
        for (GSym *g = C.gfirst; g; g = g->lnext) {
            int local = g->hidden && (g->state == GS_DEF || g->state == GS_SCRIPT);
            if (local != (pass == 0)) continue;
            if (g->state == GS_UNDEF && g->wref) {
                put_sym(tab, &n, str, slen, g->name, (uint8_t)(STB_WEAK << 4), 0, 0, 0);
                continue;
            }
            if (g->state != GS_DEF && g->state != GS_SCRIPT) continue;
            uint16_t sh = SHN_ABS;
            uint64_t val = g->value;
            if (g->state == GS_DEF && g->sec) {
                if (!g->sec->out || !g->sec->live) continue;
                val = g->sec->addr + g->value;
                sh = (uint16_t)g->sec->out->shndx;
            } else if (g->state == GS_SCRIPT) {
                KOut *ow = g->owner;
                if (!ow && !g->inside) ow = firstout;
                if (ow && ow->nonempty) sh = (uint16_t)ow->shndx;
            }
            unsigned bind = local ? STB_LOCAL : g->weak ? STB_WEAK : STB_GLOBAL;
            put_sym(tab, &n, str, slen, g->name, (uint8_t)((bind << 4) | (g->state == GS_DEF ? g->type : STT_NOTYPE)), sh, val, g->state == GS_DEF ? g->size : 0);
        }
    }
    return n;
}

static void write_phdr(uint8_t *p, uint32_t type, uint32_t flags, uint64_t off, uint64_t va, uint64_t fsz, uint64_t msz, uint64_t align) {
    w32(p, type); w32(p + 4, flags); w64(p + 8, off); w64(p + 16, va); w64(p + 24, va); w64(p + 32, fsz); w64(p + 40, msz); w64(p + 48, align);
}

static void write_shdr(uint8_t *p, uint32_t name, uint32_t type, uint64_t flags, uint64_t addr, uint64_t off, uint64_t size, uint32_t link, uint32_t info,
                       uint64_t align, uint64_t entsize) {
    w32(p, name); w32(p + 4, type); w64(p + 8, flags); w64(p + 16, addr); w64(p + 24, off); w64(p + 32, size);
    w32(p + 40, link); w32(p + 44, info); w64(p + 48, align); w64(p + 56, entsize);
}

static void produce(void) {
    Seg seg[MAX_PHDRS + 1];
    memset(seg, 0, sizeof seg);
    int ns = 0;
    /* точка входа */
    const char *en = C.entry;
    GSym *eg = gfind(en);
    if (!eg || (eg->state != GS_DEF && eg->state != GS_SCRIPT)) fail("entry symbol '%s' is not defined", en);
    uint64_t entry = 0;
    if (eg->state == GS_SCRIPT) entry = eg->value;
    else if (!eg->sec) entry = eg->value;
    else if (!eg->sec->out || !eg->sec->live) fail("entry symbol '%s' is in a discarded section", en);
    else entry = eg->sec->addr + eg->value;
    int nouts = 0;
    for (KOut *o = C.outs; o; o = o->next) o->shndx = ++nouts;
    if (nouts == 0) fail("nothing to link: the linker script placed no sections");
    ns = build_segments(seg);
    if (ns > MAX_PHDRS) fail("too many segments");
    int nph = ns + (C.have_phdrs ? 0 : 1);
    /* смещения в файле: смещение сегмента сравнимо с адресом по модулю страницы (как у ld.lld) */
    uint64_t cur = 64 + 56ULL * (uint64_t)nph;
    for (int i = 0; i < ns; i++) {
        Seg *s = &seg[i];
        uint64_t m = C.pagesz, want = s->va % m, have = cur % m;
        s->off = cur - have + want + (want < have ? m : 0);
        for (KOut *o = s->first;; o = o->next) {
            o->off = s->off + (o->va - s->va);
            if (o->nobits && o->va - s->va > s->filesz) o->off = s->off + s->filesz;   /* NOBITS не занимает файл: смещение - конец данных сегмента */
            if (o == s->last) break;
        }
        cur = s->off + s->filesz;
        if (cur > MAX_IMAGE) fail("output image is too large");
    }
    /* .comment: конкатенация, если скрипт её не отбрасывает */
    uint64_t cm_len = 0, cm_off = 0;
    for (unsigned oi = 0; oi < C.nobjs; oi++)
        for (unsigned i = 1; i < C.objs[oi].nsec; i++) {
            KSec *s = &C.objs[oi].sec[i];
            if (C.comment_first && !(s->flags & SHF_ALLOC) && !s->stmt && strcmp(s->name, ".comment") == 0) cm_len += s->size;
        }
    cm_off = cur;
    cur += cm_len;
    /* таблица символов */
    size_t slen, firstg;
    size_t nsym = emit_symbols(NULL, NULL, &slen, &firstg);
    uint64_t symoff = alup(cur, 8);
    uint64_t stroff = symoff + nsym * 24;
    char shn[128];
    size_t shl = 1;
    shn[0] = 0;
    uint32_t nm_comment = 0, nm_symtab, nm_strtab, nm_shstr;
    if (cm_len) { nm_comment = (uint32_t)shl; memcpy(shn + shl, ".comment", 9); shl += 9; }
    nm_symtab = (uint32_t)shl; memcpy(shn + shl, ".symtab", 8); shl += 8;
    nm_strtab = (uint32_t)shl; memcpy(shn + shl, ".strtab", 8); shl += 8;
    nm_shstr = (uint32_t)shl; memcpy(shn + shl, ".shstrtab", 10); shl += 10;
    size_t outnames = 0;
    for (KOut *o = C.outs; o; o = o->next) outnames += strlen(o->name) + 1;
    char *shnames = anew(shl + outnames + 1);
    memcpy(shnames, shn, shl);
    uint32_t *nmo = anew(sizeof(uint32_t) * (size_t)(nouts + 1));
    {
        size_t at = shl;
        int k = 0;
        for (KOut *o = C.outs; o; o = o->next) {
            nmo[k++] = (uint32_t)at;
            size_t l = strlen(o->name) + 1;
            memcpy(shnames + at, o->name, l);
            at += l;
        }
        shl = at;
    }
    uint64_t shstroff = stroff + slen;
    uint64_t shoff = alup(shstroff + shl, 8);
    int nsh = 1 + nouts + (cm_len ? 1 : 0) + 3;
    uint64_t total = shoff + 64ULL * (uint64_t)nsh;
    if (total > MAX_IMAGE) fail("output image is too large");
    uint8_t *img = anew((size_t)total);
    /* содержимое секций */
    for (KOut *o = C.outs; o; o = o->next) {
        if (o->nobits) continue;
        if (o->flags & SHF_EXEC) memset(img + o->off, 0xcc, (size_t)o->size);   /* промежутки в коде - int3, как у ld.lld */
        for (SStmt *st = o->st->body; st; st = st->next)
            for (KSec *s = st->kind == S_INPUT ? st->head : NULL; s; s = s->snext)
                if (s->out == o && s->data && s->type != SHT_NOBITS) memcpy(img + o->off + s->off, s->data, (size_t)s->size);
    }
    for (SStmt *it = C.items; it; it = it->next) {
        if (it->kind != S_OUT || !it->out) continue;
        for (SStmt *st = it->body; st; st = st->next)
            for (KSec *s = st->kind == S_INPUT ? st->head : NULL; s; s = s->snext)
                if (s->out && s->nrela) reloc_section(s, img);
    }
    if (cm_len) {
        uint64_t at = cm_off;
        for (unsigned oi = 0; oi < C.nobjs; oi++)
            for (unsigned i = 1; i < C.objs[oi].nsec; i++) {
                KSec *s = &C.objs[oi].sec[i];
                if (!(s->flags & SHF_ALLOC) && !s->stmt && strcmp(s->name, ".comment") == 0 && s->data) { memcpy(img + at, s->data, (size_t)s->size); at += s->size; }
            }
    }
    size_t slen2, fg2;
    size_t n2 = emit_symbols(img + symoff, img + stroff, &slen2, &fg2);
    if (n2 != nsym || slen2 != slen) fail("internal: symbol table size changed");
    memcpy(img + shstroff, shnames, shl);
    /* заголовки */
    img[0] = 0x7f; img[1] = 'E'; img[2] = 'L'; img[3] = 'F'; img[4] = 2; img[5] = 1; img[6] = 1;
    w16(img + 16, 2); w16(img + 18, 62); w32(img + 20, 1);
    w64(img + 24, entry); w64(img + 32, 64); w64(img + 40, shoff);
    w16(img + 52, 64); w16(img + 54, 56); w16(img + 56, (uint16_t)nph); w16(img + 58, 64); w16(img + 60, (uint16_t)nsh); w16(img + 62, (uint16_t)(nsh - 2));
    uint8_t *ph = img + 64;
    for (int i = 0; i < ns; i++, ph += 56) write_phdr(ph, 1, seg[i].flags, seg[i].off, seg[i].va, seg[i].filesz, seg[i].memsz, C.pagesz);
    if (!C.have_phdrs) write_phdr(ph, 0x6474e551, 6, 0, 0, 0, 0, 0);
    uint8_t *sh = img + shoff + 64;
    for (KOut *o = C.outs; o; o = o->next, sh += 64) {
        write_shdr(sh, nmo[o->shndx - 1], o->nobits ? SHT_NOBITS : SHT_PROGBITS, o->flags, o->va, o->off, o->size, 0, 0, o->align, 0);
    }
    if (cm_len) { write_shdr(sh, nm_comment, SHT_PROGBITS, 0x30, 0, cm_off, cm_len, 0, 0, 1, 1); sh += 64; }
    write_shdr(sh, nm_symtab, SHT_SYMTAB, 0, 0, symoff, nsym * 24, (uint32_t)(nsh - 1), (uint32_t)firstg, 8, 24); sh += 64;
    write_shdr(sh, nm_shstr, SHT_STRTAB, 0, 0, shstroff, shl, 0, 0, 1, 0); sh += 64;
    write_shdr(sh, nm_strtab, SHT_STRTAB, 0, 0, stroff, slen, 0, 0, 1, 0);
    C.k->out = img;
    C.k->out_len = (size_t)total;
}

/* ---------- главный вход ---------- */
static void run(KLink *k) {
    if (!k->script || !k->objs || k->nobjs == 0) fail("no input: object files and a linker script are required");
    C.pagesz = k->page_size ? k->page_size : 4096;
    if (C.pagesz & (C.pagesz - 1) || C.pagesz < 16 || C.pagesz > (1ULL << 30)) fail("bad max-page-size %llu", (unsigned long long)C.pagesz);
    C.objs = anew(sizeof(KObj) * k->nobjs);
    C.nobjs = k->nobjs;
    for (unsigned i = 0; i < k->nobjs; i++) {
        C.objs[i].name = k->objs[i].name ? k->objs[i].name : "(input)";
        C.objs[i].d = k->objs[i].data;
        C.objs[i].n = k->objs[i].len;
        parse_obj(&C.objs[i]);
    }
    resolve_symbols();
    parse_script(k->script, k->script_len);
    if (k->entry) C.entry = k->entry;
    if (!C.entry) C.entry = "_start";
    bind_sections();
    if (k->gc_sections) mark_gc();
    else
        for (unsigned oi = 0; oi < C.nobjs; oi++)
            for (unsigned i = 1; i < C.objs[oi].nsec; i++) C.objs[oi].sec[i].live = C.objs[oi].sec[i].cand && C.objs[oi].sec[i].stmt && !C.objs[oi].sec[i].discard;
    if (C.commonsec) C.commonsec->live = C.commonsec->stmt && !C.commonsec->discard && (!k->gc_sections || C.commonsec->live);
    layout();
    produce();
    k->sections_in = C.nsec_in;
    k->sections_gc = C.nsec_gc;
}

int klink_link(KLink *k) {
    memset(&C, 0, sizeof C);
    C.k = k;
    C.ab = k->arena;
    C.acap = k->arena_cap;
    KL_UNPOISON(k->arena, k->arena_cap);
    k->out = NULL;
    k->out_len = 0;
    k->err[0] = 0;
    k->sections_in = k->sections_gc = 0;
    if (setjmp(C.jb)) return -1;
    run(k);
    return 0;
}
