/* wasm - интерпретатор WebAssembly (программа Ring 3 / EL0): один файл .wasm работает на x86-64 и ARM64.
   Модуль выполняется внутри этого процесса - изоляция та же, что у любой программы Киры, ядро о WASM не знает.
   Поддержано: WebAssembly 1.0 (типы, импорт функций, функции, таблица и call_indirect, память, глобальные,
   экспорт, данные, элементы) с целыми i32/i64, расширение знака (0xC0-0xC4), memory.copy/fill. Вещественная
   арифметика - останов с сообщением (загрузка/запись/константы f32/f64 как биты - работают).
   Импорт модуля "kira": write(ptr,len), sleep(ticks), ticks()->i64, exit(code), gfx_create(w,h)->i32,
   gfx_present(ptr) (пиксели 0x00RRGGBB из памяти модуля), gfx_event(ptr)->i32 (16 байт: type,a,b,c).
   Запуск: wasm /путь/к/модулю.wasm (или двойной щелчок по .wasm в Проводнике). */
#include "kira.h"
KIRA_ENTRY()

#define MAXF 1024
#define MAXT 128
#define MAXG 64
#define VSTK (64 * 1024)
#define LSTK 4096
#define DEPTH 400
#define MAXMEM (2u << 20)                     /* память модуля - до 2 МБ (32 страницы WASM) */

static void say(const char* s) { k_write(s, k_strlen(s)); }
static void say_u(u64 v) { char t[21]; int i = 20; t[i] = 0; do { t[--i] = (char)('0' + v % 10); v /= 10; } while (v); say(t + i); }

/* ---------- память процесса: куски кучи подряд ---------- */
static u8* heap_end; static u64 heap_left;
static void* alloc(u64 n) {
    n = (n + 15) & ~15ull;
    if (n > heap_left) {
        u64 pages = (n - heap_left + 4095) / 4096;
        u8* p = (u8*)k_heap_grow(pages);
        if (!p) return 0;
        if (!heap_end) heap_end = p;
        heap_left += pages * 4096;
    }
    void* r = heap_end; heap_end += n; heap_left -= n;
    for (u64 i = 0; i < n; i++) ((u8*)r)[i] = 0;
    return r;
}

/* ---------- ошибки ---------- */
static const char* trap_msg;
static int trapped;
static void trap(const char* m) { if (!trapped) { trapped = 1; trap_msg = m; } }

/* ---------- модуль ---------- */
typedef struct { u8 np, nr; u8 p[32]; } ftype_t;
typedef struct { u32 type; int import; u32 imp_id; const u8* code; const u8* end; u32 nlocals; } func_t;
static ftype_t* types; static u32 ntypes;
static func_t* funcs; static u32 nfuncs, nimports;
static u32* table; static u32 ntable;
static u8* mem; static u32 mem_pages, mem_max;
static u64 globals[MAXG]; static u32 nglobals;
static u32 entry = ~0u;
static const u8* mod; static u32 modlen;

enum { IMP_WRITE, IMP_SLEEP, IMP_TICKS, IMP_EXIT, IMP_GFX_CREATE, IMP_GFX_PRESENT, IMP_GFX_EVENT, IMP_N };
static const char* const imp_names[IMP_N] = { "write", "sleep", "ticks", "exit", "gfx_create", "gfx_present", "gfx_event" };

static u32 leb_u(const u8** p, const u8* end) {
    u32 r = 0; int s = 0;
    while (*p < end) { u8 b = *(*p)++; r |= (u32)(b & 0x7F) << s; s += 7; if (!(b & 0x80)) return r; if (s > 35) break; }
    trap("bad LEB128"); return 0;
}
static i64 leb_s(const u8** p, const u8* end, int bits) {
    u64 r = 0; int s = 0; u8 b = 0;                               /* беззнаково: сдвиги определены */
    while (*p < end) { b = *(*p)++; if (s < 64) r |= (u64)(b & 0x7F) << s; s += 7; if (!(b & 0x80)) break; if (s > 70) { trap("bad LEB128"); return 0; } }
    if (s < 64 && (b & 0x40)) r |= ~0ull << s;
    if (bits == 32) return (i64)(int)(u32)r;
    return (i64)r;
}
static int name_is(const u8* n, u32 len, const char* s) { u32 i = 0; for (; i < len && s[i]; i++) if (n[i] != (u8)s[i]) return 0; return i == len && !s[i]; }

/* Постоянное выражение (смещение данных/элементов, начало глобальной). */
static u64 const_expr(const u8** p, const u8* end) {
    u64 v = 0;
    while (*p < end) {
        u8 op = *(*p)++;
        if (op == 0x0B) return v;
        if (op == 0x41) v = (u64)(u32)leb_s(p, end, 32);
        else if (op == 0x42) v = (u64)leb_s(p, end, 64);
        else if (op == 0x23) { u32 g = leb_u(p, end); v = g < nglobals ? globals[g] : 0; }
        else if (op == 0x43) { if (*p + 4 > end) break; v = (u64)((*p)[0] | (*p)[1] << 8 | (*p)[2] << 16 | (u32)(*p)[3] << 24); *p += 4; }
        else if (op == 0x44) { if (*p + 8 > end) break; v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | (*p)[i]; *p += 8; }
        else { trap("unsupported constant expression"); return 0; }
    }
    trap("unterminated constant expression"); return 0;
}

/* ---------- таблица блоков: позиция block/loop/if -> else и end ---------- */
typedef struct { u32 pos, els, end; } blk_t;
static blk_t* blks; static u32 nblks, cap_blks;

static void skip_imm(u8 op, const u8** p, const u8* e) {
    if (op == 0x0C || op == 0x0D || op == 0x10 || (op >= 0x20 && op <= 0x24)) { leb_u(p, e); return; }
    if (op == 0x0E) { u32 n = leb_u(p, e); for (u32 i = 0; i <= n && !trapped; i++) leb_u(p, e); return; }
    if (op == 0x11) { leb_u(p, e); leb_u(p, e); return; }
    if (op >= 0x28 && op <= 0x3E) { leb_u(p, e); leb_u(p, e); return; }
    if (op == 0x3F || op == 0x40) { (*p)++; return; }
    if (op == 0x41) { leb_s(p, e, 32); return; }
    if (op == 0x42) { leb_s(p, e, 64); return; }
    if (op == 0x43) { *p += 4; return; }
    if (op == 0x44) { *p += 8; return; }
    if (op == 0x1C) { u32 n = leb_u(p, e); *p += n; return; }
    if (op == 0xFC) {
        u32 sub = leb_u(p, e);
        if (sub == 8) { leb_u(p, e); (*p)++; } else if (sub == 9) leb_u(p, e);
        else if (sub == 10) *p += 2; else if (sub == 11) (*p)++;
        else if (sub >= 12) trap("unsupported 0xFC instruction");
    }
}

static int scan_blocks(const u8* code, const u8* end) {
    u32 stack[256]; int sp = 0;
    const u8* p = code;
    while (p < end && !trapped) {
        u32 pos = (u32)(p - mod);
        u8 op = *p++;
        if (op == 0x02 || op == 0x03 || op == 0x04) {
            u8 bt = *p;
            if (bt == 0x40 || bt == 0x7F || bt == 0x7E || bt == 0x7D || bt == 0x7C) p++;
            else leb_s(&p, end, 64);                              /* тип-индекс (многозначные блоки) */
            if (nblks == cap_blks) { trap("too many blocks"); return -1; }
            blks[nblks].pos = pos; blks[nblks].els = 0; blks[nblks].end = 0;
            if (sp == 256) { trap("blocks nested too deep"); return -1; }
            stack[sp++] = nblks++;
        } else if (op == 0x05) {
            if (sp) blks[stack[sp - 1]].els = (u32)(p - mod);
        } else if (op == 0x0B) {
            if (sp) blks[stack[--sp]].end = (u32)(p - mod - 1);
        } else skip_imm(op, &p, end);
    }
    return 0;
}
static const blk_t* blk_at(u32 pos) {
    u32 lo = 0, hi = nblks;
    while (lo < hi) { u32 m = (lo + hi) / 2; if (blks[m].pos < pos) lo = m + 1; else hi = m; }
    return lo < nblks && blks[lo].pos == pos ? &blks[lo] : 0;
}

/* ---------- загрузка ---------- */
static int load(void) {
    if (modlen < 8 || mod[0] || mod[1] != 'a' || mod[2] != 's' || mod[3] != 'm' || mod[4] != 1) { trap("not a WebAssembly 1.0 module"); return -1; }
    types = (ftype_t*)alloc(sizeof(ftype_t) * 256); funcs = (func_t*)alloc(sizeof(func_t) * MAXF);
    cap_blks = 65536; blks = (blk_t*)alloc(sizeof(blk_t) * cap_blks);
    if (!types || !funcs || !blks) { trap("no memory"); return -1; }
    const u8* p = mod + 8; const u8* end = mod + modlen;
    u32 nfuncdecl = 0; u32* decl = 0;
    while (p < end && !trapped) {
        u8 id = *p++; u32 sz = leb_u(&p, end);
        const u8* s = p; const u8* se = p + sz;
        if (se > end) { trap("section past end"); return -1; }
        if (id == 1) {                                            /* типы */
            u32 n = leb_u(&s, se);
            for (u32 i = 0; i < n && !trapped; i++) {
                if (ntypes == 256 || *s++ != 0x60) { trap("bad type"); return -1; }
                ftype_t* t = &types[ntypes++];
                u32 np = leb_u(&s, se); if (np > 32) { trap("too many params"); return -1; }
                t->np = (u8)np; for (u32 k = 0; k < np; k++) t->p[k] = *s++;
                u32 nr = leb_u(&s, se); if (nr > 1) { trap("multi-value results not supported"); return -1; }
                t->nr = (u8)nr; for (u32 k = 0; k < nr; k++) s++;
            }
        } else if (id == 2) {                                     /* импорт */
            u32 n = leb_u(&s, se);
            for (u32 i = 0; i < n && !trapped; i++) {
                u32 ml = leb_u(&s, se); const u8* mn = s; s += ml;
                u32 nl = leb_u(&s, se); const u8* nn = s; s += nl;
                u8 kind = *s++;
                if (kind == 0) {
                    u32 ti = leb_u(&s, se);
                    int found = -1;
                    for (int k = 0; k < IMP_N; k++) if ((name_is(mn, ml, "kira") || name_is(mn, ml, "env")) && name_is(nn, nl, imp_names[k])) found = k;
                    if (found < 0) { trap("unknown import (module kira: write, sleep, ticks, exit, gfx_create, gfx_present, gfx_event)"); return -1; }
                    if (nfuncs == MAXF) { trap("too many functions"); return -1; }
                    funcs[nfuncs].type = ti; funcs[nfuncs].import = 1; funcs[nfuncs].imp_id = (u32)found; nfuncs++; nimports++;
                } else if (kind == 2) {                           /* импорт памяти: создаём свою */
                    u8 fl = *s++; u32 mn2 = leb_u(&s, se); if (fl & 1) leb_u(&s, se);
                    mem_pages = mn2;
                } else { trap("only function and memory imports are supported"); return -1; }
            }
        } else if (id == 3) {                                     /* функции */
            nfuncdecl = leb_u(&s, se);
            decl = (u32*)alloc(4ull * (nfuncdecl + 1));
            for (u32 i = 0; i < nfuncdecl && !trapped; i++) decl[i] = leb_u(&s, se);
        } else if (id == 4) {                                     /* таблица */
            u32 n = leb_u(&s, se);
            if (n) { s++; u8 fl = *s++; ntable = leb_u(&s, se); if (fl & 1) leb_u(&s, se); if (ntable > 65536) ntable = 65536;
                     table = (u32*)alloc(4ull * (ntable + 1)); for (u32 i = 0; i < ntable; i++) table[i] = ~0u; }
        } else if (id == 5) {                                     /* память */
            u32 n = leb_u(&s, se);
            if (n) { u8 fl = *s++; mem_pages = leb_u(&s, se); mem_max = (fl & 1) ? leb_u(&s, se) : MAXMEM / 65536; }
        } else if (id == 6) {                                     /* глобальные */
            u32 n = leb_u(&s, se);
            for (u32 i = 0; i < n && !trapped; i++) { s++; s++; if (nglobals == MAXG) { trap("too many globals"); return -1; } globals[nglobals++] = const_expr(&s, se); }
        } else if (id == 7) {                                     /* экспорт: точка входа */
            u32 n = leb_u(&s, se);
            for (u32 i = 0; i < n && !trapped; i++) {
                u32 nl = leb_u(&s, se); const u8* nn = s; s += nl;
                u8 kind = *s++; u32 idx = leb_u(&s, se);
                if (kind == 0 && (name_is(nn, nl, "main") || name_is(nn, nl, "_start") || name_is(nn, nl, "kmain"))) entry = idx;
            }
        } else if (id == 8) {
            entry = leb_u(&s, se);
        } else if (id == 9) {                                     /* элементы таблицы */
            u32 n = leb_u(&s, se);
            for (u32 i = 0; i < n && !trapped; i++) {
                u32 fl = leb_u(&s, se);
                if (fl != 0) { trap("only active element segments (flag 0) are supported"); return -1; }
                u32 off = (u32)const_expr(&s, se), cnt = leb_u(&s, se);
                for (u32 k = 0; k < cnt; k++) { u32 f = leb_u(&s, se); if (off + k < ntable) table[off + k] = f; }
            }
        } else if (id == 10) {                                    /* код */
            u32 n = leb_u(&s, se);
            if (n != nfuncdecl) { trap("function and code counts differ"); return -1; }
            if (mem_max == 0) mem_max = MAXMEM / 65536;
            for (u32 i = 0; i < n && !trapped; i++) {
                u32 bs = leb_u(&s, se); const u8* b = s; const u8* be = s + bs;
                u32 groups = leb_u(&b, be), nl = 0;
                for (u32 g = 0; g < groups; g++) { u32 c = leb_u(&b, be); b++; nl += c; if (nl > 4096) { trap("too many locals"); return -1; } }
                if (nfuncs == MAXF) { trap("too many functions"); return -1; }
                func_t* f = &funcs[nfuncs++];
                f->type = decl[i]; f->import = 0; f->code = b; f->end = be; f->nlocals = nl;
                if (scan_blocks(b, be)) return -1;
                s = be;
            }
        } else if (id == 11) {                                    /* данные - после выделения памяти */
            if (!mem) {
                if (mem_max * 65536ull > MAXMEM) mem_max = MAXMEM / 65536;
                if (mem_pages > mem_max) { trap("module needs more memory than 2 MB"); return -1; }
                mem = (u8*)alloc((u64)mem_max * 65536);
                if (!mem) { trap("no memory for the module"); return -1; }
            }
            u32 n = leb_u(&s, se);
            for (u32 i = 0; i < n && !trapped; i++) {
                u32 fl = leb_u(&s, se);
                if (fl == 2) leb_u(&s, se);
                if (fl == 1) { trap("passive data segments are not supported"); return -1; }
                u32 off = (u32)const_expr(&s, se), len = leb_u(&s, se);
                if ((u64)off + len > (u64)mem_pages * 65536 || s + len > se) { trap("data segment out of memory"); return -1; }
                for (u32 k = 0; k < len; k++) mem[off + k] = s[k];
                s += len;
            }
        }
        p = se;
    }
    if (!trapped && !mem) {
        if (mem_max * 65536ull > MAXMEM) mem_max = MAXMEM / 65536;
        if (mem_pages > mem_max) { trap("module needs more memory than 2 MB"); return -1; }
        mem = (u8*)alloc((u64)(mem_max ? mem_max : 1) * 65536);
    }
    if (!trapped && entry == ~0u) trap("no exported main/_start");
    return trapped ? -1 : 0;
}

/* ---------- выполнение ---------- */
static u64* vs; static u32 sp;                   /* стек значений */
typedef struct { u32 cont, height; u8 arity, loop; } label_t;
static label_t* ls; static u32 lp;
static int depth, exit_req, exit_code;
static u32* fb; static u32 fbw, fbh;

#define PUSH(v) do { if (sp >= VSTK) { trap("value stack overflow"); return; } vs[sp++] = (u64)(v); } while (0)
#define POP() (vs[--sp])
#define MEMCHK(a, n) if ((u64)(a) + (n) > (u64)mem_pages * 65536) { trap("memory access out of bounds"); return; }

static u64 rd(u32 a, int n) { u64 v = 0; for (int i = n - 1; i >= 0; i--) v = v << 8 | mem[a + (u32)i]; return v; }
static void wrm(u32 a, u64 v, int n) { for (int i = 0; i < n; i++) { mem[a + (u32)i] = (u8)v; v >>= 8; } }
static u32 clz32(u32 x) { u32 n = 0; if (!x) return 32; while (!(x & 0x80000000u)) { x <<= 1; n++; } return n; }
static u32 ctz32(u32 x) { u32 n = 0; if (!x) return 32; while (!(x & 1)) { x >>= 1; n++; } return n; }
static u32 pop32(u32 x) { u32 n = 0; while (x) { n += x & 1; x >>= 1; } return n; }
static u64 clz64(u64 x) { u64 n = 0; if (!x) return 64; while (!(x >> 63)) { x <<= 1; n++; } return n; }
static u64 ctz64(u64 x) { u64 n = 0; if (!x) return 64; while (!(x & 1)) { x >>= 1; n++; } return n; }
static u64 pop64(u64 x) { u64 n = 0; while (x) { n += x & 1; x >>= 1; } return n; }

static void call(u32 fi);

static void host_call(u32 id, const ftype_t* t) {
    u64 a[4] = { 0, 0, 0, 0 };
    for (int i = t->np - 1; i >= 0; i--) { u64 v = POP(); if (i < 4) a[i] = v; }
    u64 r = 0;
    if (id == IMP_WRITE) { u32 p = (u32)a[0], n = (u32)a[1]; MEMCHK(p, n); k_write((const char*)mem + p, n); r = n; }
    else if (id == IMP_SLEEP) k_sleep(a[0]);
    else if (id == IMP_TICKS) r = k_ticks();
    else if (id == IMP_EXIT) { exit_req = 1; exit_code = (int)a[0]; }
    else if (id == IMP_GFX_CREATE) { fbw = (u32)a[0]; fbh = (u32)a[1]; fb = (fbw && fbh && fbw <= 1024 && fbh <= 768) ? k_gfx_create(fbw, fbh) : 0; r = fb ? 0 : (u64)-1; }
    else if (id == IMP_GFX_PRESENT) {
        u32 p = (u32)a[0];
        if (!fb) r = (u64)-1;
        else { MEMCHK(p, (u64)fbw * fbh * 4); for (u32 i = 0; i < fbw * fbh; i++) fb[i] = (u32)rd(p + 4 * i, 4); k_gfx_present(); }
    } else if (id == IMP_GFX_EVENT) {
        u32 p = (u32)a[0]; kevent_t e; MEMCHK(p, 16);
        int got = k_gfx_event(&e);
        if (got > 0) { wrm(p, e.type, 4); wrm(p + 4, e.a, 4); wrm(p + 8, e.b, 4); wrm(p + 12, e.c, 4); }
        r = (u64)(i64)got;
    }
    if (t->nr) PUSH(r);
}

/* Переход к метке n (0 - ближайшая): у блока - за его конец с результатами, у цикла - в начало; за пределы
   меток функции - возврат. */
static void branch(u32 n, const u8** p, const u8* end, u32 lbase) {
    if (n >= lp - lbase) { *p = end; return; }
    label_t* L = &ls[lp - 1 - n];
    u32 ar = L->arity;
    for (u32 i = 0; i < ar; i++) vs[L->height + i] = vs[sp - ar + i];
    sp = L->height + ar; *p = mod + L->cont;
    lp = (u32)(L - ls) + (L->loop ? 1 : 0);
}

static void run(const func_t* f, u32 base) {
    const u8* p = f->code; const u8* end = f->end;
    u32 lbase = lp;
    const ftype_t* ft = &types[f->type];
    while (p < end && !trapped && !exit_req) {
        u32 pos = (u32)(p - mod);
        u8 op = *p++;
        switch (op) {
        case 0x00: trap("unreachable executed"); return;
        case 0x01: break;
        case 0x02: case 0x03: case 0x04: {
            const blk_t* b = blk_at(pos);
            u8 bt = *p; u8 ar = 0;
            if (bt == 0x40) p++;
            else if (bt == 0x7F || bt == 0x7E || bt == 0x7D || bt == 0x7C) { p++; ar = 1; }
            else { i64 ti = leb_s(&p, end, 64); if (ti < 0 || (u32)ti >= ntypes || types[ti].np) { trap("block with parameters not supported"); return; } ar = types[ti].nr; }
            if (!b || !b->end) { trap("block without end"); return; }
            if (lp >= LSTK) { trap("label stack overflow"); return; }
            u32 h = sp;
            if (op == 0x04) {
                h = --sp; u32 c = (u32)vs[sp];
                if (!c) { if (b->els) p = mod + b->els; else { p = mod + b->end + 1; break; } }
            }
            label_t* L = &ls[lp++];
            L->loop = op == 0x03; L->arity = op == 0x03 ? 0 : ar; L->height = h;
            L->cont = op == 0x03 ? (u32)(p - mod) : b->end + 1;
            break;
        }
        case 0x05: {                                              /* else: «then» закончилась - к концу if */
            if (lp == lbase) { trap("else without if"); return; }
            label_t* L = &ls[--lp];
            u32 ar = L->arity; for (u32 i = 0; i < ar; i++) vs[L->height + i] = vs[sp - ar + i];
            sp = L->height + ar; p = mod + L->cont;
            break;
        }
        case 0x0B:
            if (lp > lbase) lp--;                                 /* конец блока: значения уже на стеке */
            else p = end;                                         /* конец функции */
            break;
        case 0x0C: { u32 n = leb_u(&p, end); branch(n, &p, end, lbase); break; }
        case 0x0D: { u32 n = leb_u(&p, end); u32 c = (u32)POP(); if (c) branch(n, &p, end, lbase); break; }
        case 0x0E: {
            u32 cnt = leb_u(&p, end), idx = (u32)POP(), sel = 0;
            for (u32 i = 0; i <= cnt && !trapped; i++) { u32 t = leb_u(&p, end); if (i == idx || (i == cnt && idx >= cnt)) sel = t; }
            branch(sel, &p, end, lbase); break;
        }
        case 0x0F: p = end; break;
        case 0x10: call(leb_u(&p, end)); break;
        case 0x11: {
            u32 ti = leb_u(&p, end); p++;
            u32 i = (u32)POP();
            if (i >= ntable || table[i] == ~0u) { trap("call_indirect: no such table entry"); return; }
            u32 fi = table[i];
            if (fi >= nfuncs) { trap("call_indirect: bad function"); return; }
            const ftype_t* a = &types[funcs[fi].type]; const ftype_t* b = &types[ti];
            if (a->np != b->np || a->nr != b->nr) { trap("call_indirect: signature mismatch"); return; }
            call(fi); break;
        }
        case 0x1A: sp--; break;
        case 0x1B: case 0x1C: {
            if (op == 0x1C) { u32 k = leb_u(&p, end); p += k; }
            u32 c = (u32)POP(); u64 b = POP(), a = POP(); PUSH(c ? a : b); break;
        }
        case 0x20: { u32 i = leb_u(&p, end); PUSH(vs[base + i]); break; }
        case 0x21: { u32 i = leb_u(&p, end); vs[base + i] = POP(); break; }
        case 0x22: { u32 i = leb_u(&p, end); vs[base + i] = vs[sp - 1]; break; }
        case 0x23: { u32 i = leb_u(&p, end); if (i >= nglobals) { trap("bad global"); return; } PUSH(globals[i]); break; }
        case 0x24: { u32 i = leb_u(&p, end); if (i >= nglobals) { trap("bad global"); return; } globals[i] = POP(); break; }
        case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: {
            leb_u(&p, end); u32 off = leb_u(&p, end);
            u64 a = (u64)(u32)POP() + off;
            static const u8 sz[14] = { 4, 8, 4, 8, 1, 1, 2, 2, 1, 1, 2, 2, 4, 4 };
            int n = sz[op - 0x28];
            if (a + (u64)n > (u64)mem_pages * 65536) { trap("memory access out of bounds"); return; }
            u64 v = rd((u32)a, n);
            switch (op) {
            case 0x28: case 0x2A: v = (u32)v; break;
            case 0x2C: v = (u32)(int)(signed char)v; break;
            case 0x2E: v = (u32)(int)(short)v; break;
            case 0x30: v = (u64)(i64)(signed char)v; break;
            case 0x32: v = (u64)(i64)(short)v; break;
            case 0x34: v = (u64)(i64)(int)v; break;
            default: break;
            }
            PUSH(v); break;
        }
        case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: {
            leb_u(&p, end); u32 off = leb_u(&p, end);
            static const u8 sz[9] = { 4, 8, 4, 8, 1, 2, 1, 2, 4 };
            u64 v = POP(); u64 a = (u64)(u32)POP() + off; int n = sz[op - 0x36];
            if (a + (u64)n > (u64)mem_pages * 65536) { trap("memory access out of bounds"); return; }
            wrm((u32)a, v, n); break;
        }
        case 0x3F: p++; PUSH(mem_pages); break;
        case 0x40: { p++; u32 d = (u32)POP(), old = mem_pages; if ((u64)mem_pages + d > mem_max) PUSH(0xFFFFFFFFu); else { mem_pages += d; PUSH(old); } break; }
        case 0x41: PUSH((u32)leb_s(&p, end, 32)); break;
        case 0x42: PUSH((u64)leb_s(&p, end, 64)); break;
        case 0x43: { u64 v = (u64)(p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24); p += 4; PUSH(v); break; }
        case 0x44: { u64 v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; p += 8; PUSH(v); break; }
        case 0x45: { u32 a = (u32)POP(); PUSH(a == 0); break; }
        case 0x50: { u64 a = POP(); PUSH(a == 0); break; }
        case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F: {
            u32 b = (u32)POP(), a = (u32)POP(); int sa = (int)a, sb = (int)b, r = 0;
            switch (op) { case 0x46: r = a == b; break; case 0x47: r = a != b; break; case 0x48: r = sa < sb; break; case 0x49: r = a < b; break;
                          case 0x4A: r = sa > sb; break; case 0x4B: r = a > b; break; case 0x4C: r = sa <= sb; break; case 0x4D: r = a <= b; break;
                          case 0x4E: r = sa >= sb; break; default: r = a >= b; }
            PUSH(r); break;
        }
        case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: {
            u64 b = POP(), a = POP(); i64 sa = (i64)a, sb = (i64)b; int r = 0;
            switch (op) { case 0x51: r = a == b; break; case 0x52: r = a != b; break; case 0x53: r = sa < sb; break; case 0x54: r = a < b; break;
                          case 0x55: r = sa > sb; break; case 0x56: r = a > b; break; case 0x57: r = sa <= sb; break; case 0x58: r = a <= b; break;
                          case 0x59: r = sa >= sb; break; default: r = a >= b; }
            PUSH(r); break;
        }
        case 0x67: { u32 a = (u32)POP(); PUSH(clz32(a)); break; }
        case 0x68: { u32 a = (u32)POP(); PUSH(ctz32(a)); break; }
        case 0x69: { u32 a = (u32)POP(); PUSH(pop32(a)); break; }
        case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: {
            u32 b = (u32)POP(), a = (u32)POP(), r = 0; int sa = (int)a, sb = (int)b;
            switch (op) {
            case 0x6A: r = a + b; break; case 0x6B: r = a - b; break; case 0x6C: r = a * b; break;
            case 0x6D: if (!b) { trap("integer divide by zero"); return; } if (sa == (int)0x80000000 && sb == -1) { trap("integer overflow"); return; } r = (u32)(sa / sb); break;
            case 0x6E: if (!b) { trap("integer divide by zero"); return; } r = a / b; break;
            case 0x6F: if (!b) { trap("integer divide by zero"); return; } r = (sb == -1) ? 0 : (u32)(sa % sb); break;
            case 0x70: if (!b) { trap("integer divide by zero"); return; } r = a % b; break;
            case 0x71: r = a & b; break; case 0x72: r = a | b; break; case 0x73: r = a ^ b; break;
            case 0x74: r = a << (b & 31); break; case 0x75: r = (u32)(sa >> (b & 31)); break; case 0x76: r = a >> (b & 31); break;
            case 0x77: r = (a << (b & 31)) | (a >> ((32 - (b & 31)) & 31)); break;
            default: r = (a >> (b & 31)) | (a << ((32 - (b & 31)) & 31)); }
            PUSH(r); break;
        }
        case 0x79: { u64 a = POP(); PUSH(clz64(a)); break; }
        case 0x7A: { u64 a = POP(); PUSH(ctz64(a)); break; }
        case 0x7B: { u64 a = POP(); PUSH(pop64(a)); break; }
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89: case 0x8A: {
            u64 b = POP(), a = POP(), r = 0; i64 sa = (i64)a, sb = (i64)b;
            switch (op) {
            case 0x7C: r = a + b; break; case 0x7D: r = a - b; break; case 0x7E: r = a * b; break;
            case 0x7F: if (!b) { trap("integer divide by zero"); return; } if (a == 0x8000000000000000ull && sb == -1) { trap("integer overflow"); return; } r = (u64)(sa / sb); break;
            case 0x80: if (!b) { trap("integer divide by zero"); return; } r = a / b; break;
            case 0x81: if (!b) { trap("integer divide by zero"); return; } r = (sb == -1) ? 0 : (u64)(sa % sb); break;
            case 0x82: if (!b) { trap("integer divide by zero"); return; } r = a % b; break;
            case 0x83: r = a & b; break; case 0x84: r = a | b; break; case 0x85: r = a ^ b; break;
            case 0x86: r = a << (b & 63); break; case 0x87: r = (u64)(sa >> (b & 63)); break; case 0x88: r = a >> (b & 63); break;
            case 0x89: r = (a << (b & 63)) | (a >> ((64 - (b & 63)) & 63)); break;
            default: r = (a >> (b & 63)) | (a << ((64 - (b & 63)) & 63)); }
            PUSH(r); break;
        }
        case 0xA7: { u64 a = POP(); PUSH((u32)a); break; }
        case 0xAC: { u64 a = POP(); PUSH((u64)(i64)(int)(u32)a); break; }
        case 0xAD: { u64 a = POP(); PUSH((u64)(u32)a); break; }
        case 0xBC: case 0xBE: { u64 a = POP(); PUSH((u32)a); break; }   /* перенос битов f32 <-> i32 */
        case 0xBD: case 0xBF: break;                                    /* f64 <-> i64: биты те же */
        case 0xC0: { u64 a = POP(); PUSH((u32)(int)(signed char)a); break; }
        case 0xC1: { u64 a = POP(); PUSH((u32)(int)(short)a); break; }
        case 0xC2: { u64 a = POP(); PUSH((u64)(i64)(signed char)a); break; }
        case 0xC3: { u64 a = POP(); PUSH((u64)(i64)(short)a); break; }
        case 0xC4: { u64 a = POP(); PUSH((u64)(i64)(int)a); break; }
        case 0xFC: {
            u32 sub = leb_u(&p, end);
            if (sub == 10) {                                      /* memory.copy */
                p += 2; u32 n = (u32)POP(), s = (u32)POP(), d = (u32)POP();
                if ((u64)s + n > (u64)mem_pages * 65536 || (u64)d + n > (u64)mem_pages * 65536) { trap("memory.copy out of bounds"); return; }
                if (d < s) for (u32 i = 0; i < n; i++) mem[d + i] = mem[s + i]; else for (u32 i = n; i > 0; i--) mem[d + i - 1] = mem[s + i - 1];
            } else if (sub == 11) {                               /* memory.fill */
                p++; u32 n = (u32)POP(), v = (u32)POP(), d = (u32)POP();
                if ((u64)d + n > (u64)mem_pages * 65536) { trap("memory.fill out of bounds"); return; }
                for (u32 i = 0; i < n; i++) mem[d + i] = (u8)v;
            } else { trap("unsupported 0xFC instruction (saturating float conversion or passive data)"); return; }
            break;
        }
        default:
            trap((op >= 0x5B && op <= 0x66) || (op >= 0x8B && op <= 0xBB) ? "floating-point arithmetic is not supported (integer WebAssembly subset)" : "unknown instruction");
            return;
        }
    }
    (void)ft;
    lp = lbase;
}

static void call(u32 fi) {
    if (fi >= nfuncs) { trap("call: bad function index"); return; }
    func_t* f = &funcs[fi]; const ftype_t* t = &types[f->type];
    if (f->import) { host_call(f->imp_id, t); return; }
    if (++depth > DEPTH) { trap("call stack exhausted"); return; }
    if (sp < t->np) { trap("stack underflow"); return; }
    u32 base = sp - t->np;
    if (sp + f->nlocals + 1024 > VSTK) { trap("value stack overflow"); return; }
    for (u32 i = 0; i < f->nlocals; i++) vs[sp++] = 0;
    run(f, base);
    if (!trapped && !exit_req) {                                  /* результат - на вершине; кадр снимается */
        u64 r = t->nr ? vs[sp - 1] : 0;
        sp = base;
        if (t->nr) vs[sp++] = r;
    }
    depth--;
}

int umain(void) {
    char path[128];
    if (k_grant_name(path, sizeof path) <= 0) { say("wasm: usage: wasm /path/module.wasm (or double click in Files)\n"); return 1; }
    i64 sz = k_grant_size();
    if (sz <= 0 || sz > (8 << 20)) { say("wasm: module not readable or larger than 8 MB\n"); return 1; }
    u8* buf = (u8*)alloc((u64)sz);
    vs = (u64*)alloc(8ull * VSTK); ls = (label_t*)alloc(sizeof(label_t) * LSTK);
    if (!buf || !vs || !ls) { say("wasm: no memory\n"); return 1; }
    u64 got = 0;
    while (got < (u64)sz) { i64 r = k_grant_read(buf + got, (u64)sz - got > 65536 ? 65536 : (u64)sz - got, got); if (r <= 0) break; got += (u64)r; }
    mod = buf; modlen = (u32)got;
    if (load() == 0) {
        say("wasm: "); say(path); say(" - "); say_u(nfuncs - nimports); say(" functions, "); say_u(nimports); say(" imports, memory ");
        say_u(mem_pages * 64); say(" KB\n");
        const ftype_t* t = &types[funcs[entry].type];
        for (u32 i = 0; i < t->np; i++) vs[sp++] = 0;              /* main(argc=0, argv=0) */
        u64 t0 = k_clock_ns();
        call(entry);
        u64 ms = (k_clock_ns() - t0) / 1000000;
        if (!trapped) {
            int code = exit_req ? exit_code : (t->nr ? (int)(u32)vs[sp - 1] : 0);
            say("wasm: finished with code "); say_u((u64)(u32)code); say(" in "); say_u(ms); say(" ms\n");
            return code;
        }
    }
    say("wasm: stopped: "); say(trap_msg ? trap_msg : "?"); say("\n");
    return 1;
}
