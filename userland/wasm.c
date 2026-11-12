/* wasm - интерпретатор WebAssembly (программа Ring 3 / EL0): один файл .wasm работает на x86-64 и ARM64.
   Модуль выполняется внутри этого процесса - изоляция та же, что у любой программы Киры, ядро о WASM не знает.
   Поддержано: WebAssembly 1.0 (типы, импорт функций, функции, таблица и call_indirect, память, глобальные,
   экспорт, данные, элементы) с целыми i32/i64 и вещественными f32/f64 (IEEE 754: сравнения, арифметика, sqrt,
   floor/ceil/trunc/nearest без libm, min/max с NaN и -0, преобразования с ловушкой и насыщающие 0xFC 0-7),
   расширение знака (0xC0-0xC4), memory.copy/fill.
   Импорт модуля "kira": write(ptr,len), sleep(ticks), ticks()->i64, exit(code), gfx_create(w,h)->i32,
   gfx_present(ptr) (пиксели 0x00RRGGBB из памяти модуля), gfx_event(ptr)->i32 (16 байт: type,a,b,c).
   Запуск: wasm /путь/к/модулю.wasm (или двойной щелчок по .wasm в Проводнике). */
#include "kira.h"
KIRA_ENTRY()

/* ---------- вещественные: значения на стеке - биты (f32 - младшие 32) ---------- */
typedef union { float f; u32 u; } F32;
typedef union { double f; u64 u; } F64;
static float  gf(u64 v) { F32 x; x.u = (u32)v; return x.f; }
static double gd(u64 v) { F64 x; x.u = v; return x.f; }
static u64 pf(float f) { F32 x; x.f = f; return x.u; }
static u64 pd(double d) { F64 x; x.f = d; return x.u; }
static int nan_d(double d) { return d != d; }
static double trunc_d(double x) {                                 /* без libm: |x| >= 2^52 - уже целое */
    if (nan_d(x) || x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;
    double t = (double)(i64)x;
    return t == 0 && pd(x) >> 63 ? -0.0 : t;
}
static double floor_d(double x) { double t = trunc_d(x); return t > x ? t - 1 : t; }
static double ceil_d(double x) { double t = trunc_d(x); double r = t < x ? t + 1 : t; return r == 0 && pd(x) >> 63 ? -0.0 : r; }
static double nearest_d(double x) {                               /* к чётному: прибавить и вычесть 2^52 */
    if (nan_d(x) || x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;
    volatile double big = 4503599627370496.0;
    double a = x < 0 ? -x : x, r = (a + big) - big;
    return pd(x) >> 63 ? -r : r;
}
static float trunc_f(float x) { return (float)trunc_d(x); }       /* f32 точно представим в f64 */
static float floor_f(float x) { return (float)floor_d(x); }
static float ceil_f(float x) { return (float)ceil_d(x); }
static float nearest_f(float x) {
    if (x != x || x >= 8388608.0f || x <= -8388608.0f) return x;
    volatile float big = 8388608.0f;
    float a = x < 0 ? -x : x, r = (a + big) - big;
    return pf(x) >> 31 ? -r : r;
}
static double min_d(double a, double b) { if (nan_d(a) || nan_d(b)) return gd(0x7FF8000000000000ull); if (a == b) return (pd(a) >> 63) ? a : b; return a < b ? a : b; }
static double max_d(double a, double b) { if (nan_d(a) || nan_d(b)) return gd(0x7FF8000000000000ull); if (a == b) return (pd(a) >> 63) ? b : a; return a > b ? a : b; }
static float min_f(float a, float b) { if (a != a || b != b) return gf(0x7FC00000u); if (a == b) return (pf(a) >> 31) ? a : b; return a < b ? a : b; }
static float max_f(float a, float b) { if (a != a || b != b) return gf(0x7FC00000u); if (a == b) return (pf(a) >> 31) ? b : a; return a > b ? a : b; }
/* Преобразование в целое: 1 - получилось (*out), 0 - NaN или вне диапазона (ловушка; насыщающий вариант - предел). */
static int to_int(double x, int bits, int sgn, u64* out) {
    if (nan_d(x)) { *out = 0; return 0; }
    double t = trunc_d(x), lo, hi;
    if (sgn) { lo = bits == 32 ? -2147483648.0 : -9223372036854775808.0; hi = bits == 32 ? 2147483648.0 : 9223372036854775808.0; }
    else { lo = 0; hi = bits == 32 ? 4294967296.0 : 18446744073709551616.0; }
    if (t < lo || t >= hi) {
        if (sgn) *out = t < lo ? (bits == 32 ? 0x80000000u : 0x8000000000000000ull) : (bits == 32 ? 0x7FFFFFFFu : 0x7FFFFFFFFFFFFFFFull);
        else *out = t < lo ? 0 : (bits == 32 ? 0xFFFFFFFFu : 0xFFFFFFFFFFFFFFFFull);
        return 0;
    }
    if (sgn) { i64 v = (i64)t; *out = bits == 32 ? (u64)(u32)(int)v : (u64)v; }
    else *out = t >= 9223372036854775808.0 ? (u64)(i64)(t - 9223372036854775808.0) + 0x8000000000000000ull : (u64)(i64)t;
    if (bits == 32) *out &= 0xFFFFFFFFu;
    return 1;
}
static double u64_to_d(u64 v) { if (!(v >> 63)) return (double)(i64)v; double h = (double)(i64)((v >> 1) | (v & 1)); return h + h; }
static float u64_to_f(u64 v) { if (!(v >> 63)) return (float)(i64)v; float h = (float)(i64)((v >> 1) | (v & 1)); return h + h; }

#define MAXF 1024
#define MAXT 128
#define MAXG 64
#define VSTK (64 * 1024)
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
typedef struct { u8 np, nr, r; u8 p[32]; } ftype_t;
typedef struct { u32 type; int import; u32 imp_id; const u8* code; const u8* end; u32 nlocals, centry, maxh; } func_t;
static ftype_t* types; static u32 ntypes;
static func_t* funcs; static u32 nfuncs, nimports;
static u32* table; static u32 ntable;
static u8* mem; static u32 mem_pages, mem_max, has_memory;
static u64 globals[MAXG]; static u8 global_mut[MAXG]; static u32 nglobals;
static u32 entry = ~0u;
static const u8* mod; static u32 modlen;

enum { IMP_WRITE, IMP_SLEEP, IMP_TICKS, IMP_EXIT, IMP_GFX_CREATE, IMP_GFX_PRESENT, IMP_GFX_EVENT, IMP_N };
static const char* const imp_names[IMP_N] = { "write", "sleep", "ticks", "exit", "gfx_create", "gfx_present", "gfx_event" };

static int wasm_valtype(u8 t) {
    return t == 0x7F || t == 0x7E || t == 0x7D || t == 0x7C;
}

/* WebAssembly names are UTF-8. Reject overlong encodings, surrogates,
   truncated continuation sequences and code points above U+10FFFF. */
static int wasm_utf8_valid(const u8* p, u32 n) {
    u32 i = 0;
    while (i < n) {
        u8 a = p[i++];
        if (a < 0x80) continue;
        if (a >= 0xC2 && a <= 0xDF) {
            if (i >= n || (p[i++] & 0xC0) != 0x80) return 0;
        } else if (a >= 0xE0 && a <= 0xEF) {
            if (i + 1 >= n) return 0;
            u8 b = p[i++], c = p[i++];
            if ((c & 0xC0) != 0x80 || (b & 0xC0) != 0x80 ||
                (a == 0xE0 && b < 0xA0) || (a == 0xED && b >= 0xA0))
                return 0;
        } else if (a >= 0xF0 && a <= 0xF4) {
            if (i + 2 >= n) return 0;
            u8 b = p[i++], c = p[i++], d = p[i++];
            if ((c & 0xC0) != 0x80 || (d & 0xC0) != 0x80 ||
                (b & 0xC0) != 0x80 || (a == 0xF0 && b < 0x90) ||
                (a == 0xF4 && b >= 0x90))
                return 0;
        } else return 0;
    }
    return 1;
}

/* Imports are a security boundary: never call a host function with a module-
   supplied signature that disagrees with the KIRA ABI. */
static int import_type_ok(u32 id, const ftype_t* t) {
    static const u8 np[IMP_N] = { 2, 1, 0, 1, 2, 1, 1 };
    static const u8 nr[IMP_N] = { 1, 0, 1, 0, 1, 1, 1 };
    static const u8 p[IMP_N][2] = {
        { 0x7F, 0x7F }, { 0x7F, 0 }, { 0, 0 }, { 0x7F, 0 },
        { 0x7F, 0x7F }, { 0x7F, 0 }, { 0x7F, 0 }
    };
    static const u8 result[IMP_N] = { 0x7F, 0, 0x7E, 0, 0x7F, 0x7F, 0x7F };
    if (id >= IMP_N || !t || t->np != np[id] || t->nr != nr[id]) return 0;
    for (u32 i = 0; i < t->np; i++) if (t->p[i] != p[id][i]) return 0;
    return !t->nr || t->r == result[id];
}

static int wasm_type_equal(const ftype_t* a, const ftype_t* b) {
    if (!a || !b || a->np != b->np || a->nr != b->nr) return 0;
    for (u32 i = 0; i < a->np; i++) if (a->p[i] != b->p[i]) return 0;
    return !a->nr || a->r == b->r;
}

/* WebAssembly LEB128 permits padded encodings, but never more than ceil(N/7)
   bytes and never non-zero/sign-extension bits outside the declared width. */
static u32 leb_u(const u8** p, const u8* end) {
    u32 r = 0;
    for (u32 i = 0; i < 5; i++) {
        if (*p >= end) break;
        u8 b = *(*p)++;
        u8 payload = b & 0x7Fu;
        if (i == 4 && (payload & 0x70u)) break;
        r |= (u32)payload << (i * 7u);
        if (!(b & 0x80u)) return r;
        if (i == 4) break;
    }
    trap("bad unsigned LEB128");
    return 0;
}
static i64 leb_s(const u8** p, const u8* end, int bits) {
    if (bits != 32 && bits != 64) {
        trap("unsupported signed LEB128 width");
        return 0;
    }
    u64 r = 0;
    u32 max_bytes = bits == 32 ? 5u : 10u;
    for (u32 i = 0; i < max_bytes; i++) {
        if (*p >= end) break;
        u8 b = *(*p)++;
        u8 payload = b & 0x7Fu;
        u32 shift = i * 7u;
        if (i + 1u == max_bytes) {
            u32 used = (u32)bits - shift;
            u8 low_mask = (u8)((1u << used) - 1u);
            u8 high_mask = (u8)(0x7Fu & (u8)~low_mask);
            int negative = (payload & (1u << (used - 1u))) != 0;
            if ((b & 0x80u) ||
                (negative ? ((payload & high_mask) != high_mask)
                          : ((payload & high_mask) != 0)))
                break;
            r |= (u64)(payload & low_mask) << shift;
            if (negative && bits < 64) r |= ~0ull << bits;
            return bits == 32 ? (i64)(int)(u32)r : (i64)r;
        }
        r |= (u64)payload << shift;
        if (!(b & 0x80u)) {
            u32 total = shift + 7u;
            if (b & 0x40u) r |= ~0ull << total;
            return bits == 32 ? (i64)(int)(u32)r : (i64)r;
        }
    }
    trap("bad signed LEB128");
    return 0;
}
static int name_is(const u8* n, u32 len, const char* s) { u32 i = 0; for (; i < len && s[i]; i++) if (n[i] != (u8)s[i]) return 0; return i == len && !s[i]; }

/* WebAssembly MVP constant expressions contain exactly one typed constant
   (or an imported immutable global, which this KIRA ABI does not expose), then end. */
static u64 const_expr(const u8** p, const u8* end, u8 expected_type) {
    if (*p >= end) { trap("truncated constant expression"); return 0; }
    u8 op = *(*p)++;
    u8 actual_type = 0;
    u64 v = 0;
    if (op == 0x41) { actual_type = 0x7F; v = (u64)(u32)leb_s(p, end, 32); }
    else if (op == 0x42) { actual_type = 0x7E; v = (u64)leb_s(p, end, 64); }
    else if (op == 0x43) {
        actual_type = 0x7D;
        if ((u64)(end - *p) < 4) { trap("truncated f32 constant expression"); return 0; }
        v = (u64)((u32)(*p)[0] | (u32)(*p)[1] << 8 | (u32)(*p)[2] << 16 | (u32)(*p)[3] << 24);
        *p += 4;
    } else if (op == 0x44) {
        actual_type = 0x7C;
        if ((u64)(end - *p) < 8) { trap("truncated f64 constant expression"); return 0; }
        for (int i = 7; i >= 0; i--) v = v << 8 | (*p)[i];
        *p += 8;
    } else if (op == 0x23) {
        u32 g = leb_u(p, end);
        if (trapped || g >= nglobals) trap("constant expression global index out of range");
        else trap("constant expression references unsupported local global");
        return 0;
    } else {
        trap("unsupported constant expression");
        return 0;
    }
    if (trapped) return 0;
    if (*p >= end || *(*p)++ != 0x0B) {
        trap("constant expression must contain one value and end");
        return 0;
    }
    if (actual_type != expected_type) {
        trap("constant expression type mismatch");
        return 0;
    }
    return v;
}

static int skip_bytes(const u8** p, const u8* e, u32 n) {
    if ((u64)(e - *p) < n) {
        trap("truncated instruction immediate");
        return -1;
    }
    *p += n;
    return 0;
}
static void skip_imm(u8 op, const u8** p, const u8* e, u32 nloc) {
    if (op == 0x0C || op == 0x0D) {
        u32 depth = leb_u(p, e);
        if (trapped || depth >= (u32)nctl) trap("branch depth out of range in unreachable code");
        return;
    }
    if (op == 0x10) {
        u32 index = leb_u(p, e);
        if (trapped || index >= nfuncs) trap("call function index out of range in unreachable code");
        return;
    }
    if (op >= 0x20 && op <= 0x22) {
        u32 index = leb_u(p, e);
        if (trapped || index >= nloc) trap("local index out of range in unreachable code");
        return;
    }
    if (op == 0x23 || op == 0x24) {
        u32 index = leb_u(p, e);
        if (trapped || index >= nglobals) trap("global index out of range in unreachable code");
        return;
    }
    if (op == 0x0E) {
        u32 n = leb_u(p, e);
        if (trapped || (u64)n + 1u > (u64)(e - *p)) {
            trap("bad br_table immediate"); return;
        }
        for (u32 i = 0; i <= n && !trapped; i++) {
            u32 depth = leb_u(p, e);
            if (!trapped && depth >= (u32)nctl) trap("br_table depth out of range in unreachable code");
        }
        return;
    }
    if (op == 0x11) {
        u32 type_index = leb_u(p, e);
        if (trapped || type_index >= ntypes) { trap("call_indirect type index out of range in unreachable code"); return; }
        if (!table || *p >= e || *(*p)++ != 0) trap("unsupported call_indirect table index");
        return;
    }
    if (op >= 0x28 && op <= 0x3E) {
        static const u8 max_align[23] = {
            2,3,2,3,0,0,1,1,0,0,1,1,2,2,2,3,2,3,0,1,0,1,2
        };
        u32 align = leb_u(p, e);
        (void)leb_u(p, e);
        if (trapped) return;
        if (!has_memory) { trap("memory instruction without memory in unreachable code"); return; }
        if (align > max_align[op - 0x28]) trap("memory alignment exceeds natural alignment");
        return;
    }
    if (op == 0x3F || op == 0x40) {
        if (!has_memory) { trap("memory size/grow without memory in unreachable code"); return; }
        if (*p >= e || *(*p)++ != 0) trap("unsupported memory index immediate");
        return;
    }
    if (op == 0x41) { (void)leb_s(p, e, 32); return; }
    if (op == 0x42) { (void)leb_s(p, e, 64); return; }
    if (op == 0x43) { (void)skip_bytes(p, e, 4); return; }
    if (op == 0x44) { (void)skip_bytes(p, e, 8); return; }
    if (op == 0x1C) {
        u32 n = leb_u(p, e);
        if (trapped || n != 1 || *p >= e || !wasm_valtype(**p)) {
            trap("unsupported typed select result type"); return;
        }
        (*p)++;
        return;
    }
    if (op == 0xFC) {
        u32 sub = leb_u(p, e);
        if (trapped) return;
        if (sub == 8) {
            (void)leb_u(p, e);
            u32 memory_index = leb_u(p, e);
            if (trapped) return;
            if (!has_memory || memory_index != 0) trap("unsupported memory.init memory index or missing memory");
            else trap("passive data segments are not supported");
        } else if (sub == 9) {
            (void)leb_u(p, e);
            if (!trapped) trap("passive data segments are not supported");
        } else if (sub == 10) {
            if (!has_memory) { trap("memory.copy without memory in unreachable code"); return; }
            u32 dst_memory = leb_u(p, e), src_memory = leb_u(p, e);
            if (trapped || dst_memory || src_memory) trap("unsupported memory.copy memory index");
        } else if (sub == 11) {
            if (!has_memory) { trap("memory.fill without memory in unreachable code"); return; }
            u32 memory_index = leb_u(p, e);
            if (trapped || memory_index != 0) trap("unsupported memory.fill memory index");
        } else if (sub >= 12) {
            trap("unsupported 0xFC instruction");
        }
        return;
    }
    /* Even unreachable code must be structurally valid. Only accept known
       no-immediate instructions here; unknown opcodes can carry immediates. */
    if (op == 0x00 || op == 0x01 || op == 0x0F || op == 0x1A || op == 0x1B ||
        (op >= 0x45 && op <= 0xC4))
        return;
    trap("unsupported instruction in unreachable code");
}

/* ---------- предкомпиляция: байты WASM -> плотный байт-код (слова u32) ----------
   Один раз при загрузке: LEB128 разобраны, переходы block/loop/if/br/br_table разрешены в индексы слов, высоты
   стека значений известны заранее (выполнение не проверяет переполнение на каждой команде, только на входе в
   функцию), соседние команды слиты в суперинструкции. Слово команды: младшие 16 бит - код, старшие - малый
   операнд (номер локальной, индекс функции); остальные операнды - следующие слова.
   «Ленивые» операнды: local.get и константы на вершине не кладутся на стек, а подставляются в потребляющую
   команду (режимы SS, SL, SK, LL, LK: стек/локальная/константа). Любая команда без такого режима, метка и
   вызов сначала материализуют их. Неизвестная команда превращается в ловушку, которая сработает при выполнении. */
#define I32_ARITH(X) \
    X(IADD, (u64)(u32)(a + b)) X(ISUB, (u64)(u32)(a - b)) X(IMUL, (u64)(u32)(a * b)) X(IAND, (u64)(a & b)) \
    X(IOR, (u64)(a | b)) X(IXOR, (u64)(a ^ b)) X(ISHL, (u64)(u32)(a << (b & 31))) \
    X(ISHRS, (u64)(u32)((int)a >> (b & 31))) X(ISHRU, (u64)(a >> (b & 31)))
#define I32_CMP(X) \
    X(IEQ, a == b) X(INE, a != b) X(ILTS, (int)a < (int)b) X(ILTU, a < b) X(IGTS, (int)a > (int)b) X(IGTU, a > b) \
    X(ILES, (int)a <= (int)b) X(ILEU, a <= b) X(IGES, (int)a >= (int)b) X(IGEU, a >= b)
#define F32_ARITH(X) X(FADD, pf(a + b)) X(FSUB, pf(a - b)) X(FMUL, pf(a * b)) X(FDIV, pf(a / b))
#define F32_CMP(X) X(FEQ, a == b) X(FNE, a != b) X(FLT, a < b) X(FGT, a > b) X(FLE, a <= b) X(FGE, a >= b)
/* одноместные с операндом-локальной: код wasm -> результат по сырому слову a */
#define UL_LIST(X) \
    X(UEQZ, (u64)((u32)a == 0)) X(UCVT, pf((float)(int)(u32)a)) X(UEXTS, (u64)(i64)(int)(u32)a) X(UEXTU, (u64)(u32)a) \
    X(UWRAP, (u64)(u32)a) X(UFABS, (u64)((u32)a & 0x7FFFFFFFu)) X(UFNEG, (u64)((u32)a ^ 0x80000000u)) X(UFSQRT, pf(__builtin_sqrtf(gf(a))))
#define LD_LIST(X) \
    X(LD32, 4, (u64)ld32(m + ea)) X(LD64, 8, ld64(m + ea)) X(LD8U, 1, (u64)m[ea]) X(LD8S, 1, (u64)(u32)(int)(signed char)m[ea]) \
    X(LD16U, 2, (u64)ld16(m + ea)) X(LD16S, 2, (u64)(u32)(int)(short)ld16(m + ea))
#define ST_LIST(X) X(ST32, 4, st32(m + ea, (u32)v)) X(ST64, 8, st64(m + ea, v)) X(ST8, 1, m[ea] = (u8)v) X(ST16, 2, st16(m + ea, (u16)v))
#define EN5(n, e) n##_SS, n##_SL, n##_SK, n##_LL, n##_LK, n##_LS, n##_KS,
#define ENB(n, e) n##B_SS, n##B_SL, n##B_SK, n##B_LL, n##B_LK, n##B_LS, n##B_KS,
#define ENFB(n, e) n##B_SS, n##B_SL, n##B_SK, n##B_LL, n##B_LK, n##B_LS, n##B_KS, n##N_SS, n##N_SL, n##N_SK, n##N_LL, n##N_LK, n##N_LS, n##N_KS,
#define ENL(n, e) n,
#define ENLD(n, s, e) n##_S, n##_L,
#define ENST(n, s, e) n##_SS, n##_SL, n##_LL,
enum { M_SS, M_SL, M_SK, M_LL, M_LK, M_LS, M_KS, NM };            /* стек/локальная/константа: SS SL SK LL LK, слева локальная или константа при правом со стека: LS KS */
enum {
    OP_FC = 0x100,                                                /* 0x100 + подкод префикса 0xFC */
    OP_PUSHL = 0x120, OP_PUSHK, OP_SET, OP_SETL, OP_SETK, OP_TEE, OP_GGET, OP_GSET,
    OP_JMP, OP_BRZ_S, OP_BRNZ_S, OP_BRZ_L, OP_BRNZ_L, OP_BRD, OP_BRTAB, OP_RET, OP_CALL, OP_CALLI, OP_TRAP, OP_INSL, OP_INSK, OP_SEL,
    I32_ARITH(EN5) I32_CMP(EN5) I32_CMP(ENB) F32_ARITH(EN5) F32_CMP(EN5) F32_CMP(ENFB) UL_LIST(ENL) LD_LIST(ENLD) ST_LIST(ENST)
    OP_END
};
#define OP_CMP0 IEQ_SS
#define OP_CMPB0 IEQB_SS
#define OP_FCMP0 FEQ_SS
#define OP_FCMPB0 FEQB_SS                                             /* на имя: 7 режимов «прыгнуть, если истина» и 7 «если ложь» */
static const u8 cmp_inv[10] = { 1, 0, 8, 9, 6, 7, 4, 5, 2, 3 };    /* отрицание i32-сравнения: lt_s <-> ge_s и т.д. */
static const u8 i32_arith_wasm[9] = { 0x6A, 0x6B, 0x6C, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76 };
static const u8 ul_wasm[8] = { 0x45, 0xB2, 0xAC, 0xAD, 0xA7, 0x8B, 0x8C, 0x91 };
static const char* const trap_msgs[] = { "unreachable executed", "unknown instruction", "bad global", "block with parameters not supported", "unsupported 0xFC instruction (passive data segments)" };
enum { TM_UNREACH, TM_UNKNOWN, TM_GLOBAL, TM_BLOCKP, TM_PASSIVE };

static u32* code; static u32 cn, ccap;
typedef struct { u8 kind, ar, dead; u32 h, start, pend, ifz; } ctl_t;      /* kind: 0 block, 1 loop, 2 if, 3 функция */
static ctl_t ctl[260]; static int nctl;
#define VK_MAX 2048
static u8 vk[VK_MAX]; static u32 vv[VK_MAX];                    /* виртуальный стек: вид элемента на высоте i (0 - на стеке, 1 - локальная, 2 - константа) и значение */
static u32 c_h, c_maxh, barrier, lc_start, lc_end, lf_start, lf_end, lp_start, lp_end; static int c_unr, c_inv;   /* lp - последняя производящая команда, lc - сравнение; c_inv - отложенное eqz перед br_if/if */

static void ew(u32 w) { if (cn >= ccap) { trap("compiled code too large"); return; } code[cn++] = w; }
static void hpush(u32 n) {
    if (c_h + n > VK_MAX) { trap("value stack too deep for one expression"); n = 0; }
    for (u32 j = 0; j < n; j++) vk[c_h + j] = 0;
    c_h += n; if (c_h > c_maxh) c_maxh = c_h;
}
static void lz_push(u8 kind, u32 v) { u32 i = c_h; hpush(1); if (i < VK_MAX && c_h > i) { vk[i] = kind; vv[i] = v; } }
/* Ленивый элемент i - на стек: значение вставляется под вышележащие элементы, которые уже лежат на стеке. */
static void mat(u32 i) {
    u32 d = 0;
    for (u32 j = i + 1; j < c_h; j++) if (!vk[j]) d++;
    if (vk[i] == 1) { ew((d ? OP_INSL : OP_PUSHL) | vv[i] << 16); if (d) ew(d); }
    else { ew(d ? OP_INSK : OP_PUSHK); ew(vv[i]); if (d) ew(d); }
    vk[i] = 0;
}
static void lz_flush(void) { for (u32 j = ctl[nctl - 1].h; j < c_h; j++) if (vk[j]) mat(j); }
static void lz_flush_below_top(void) { for (u32 j = ctl[nctl - 1].h; j + 1 < c_h; j++) if (vk[j]) mat(j); }
static void ref_fwd(ctl_t* f, u32 idx) { code[idx] = f->pend; f->pend = idx + 1; }   /* цепочка слов-целей до конца блока */
static void patch_chain(u32 chain, u32 target) { while (chain) { u32 i = chain - 1, nx = code[i]; code[i] = target; chain = nx; } }
static int fusable_cmp(void) { return c_h > ctl[nctl - 1].h && !vk[c_h - 1] && lc_end == cn && barrier != cn; }

/* Условная ветка по значению на вершине: nz - прыгать, если не ноль (иначе - если ноль). Возвращает индекс слова-цели. */
static u32 cond_branch(int nz) {
    nz ^= c_inv; c_inv = 0;
    u32 t = c_h - 1, lazy_below = 0;
    for (u32 j = ctl[nctl - 1].h; j < t; j++) if (vk[j]) lazy_below = 1;
    if (!vk[t] && !lazy_below && fusable_cmp()) {                 /* сравнение + переход в одной команде */
        u32 op = code[lc_start], k = (op & 0xFFFF) - OP_CMP0, kk = k / NM, mode = k % NM;
        if (!nz) kk = cmp_inv[kk];
        code[lc_start] = (OP_CMPB0 + kk * NM + mode) | (op & 0xFFFF0000u);
        u32 i = cn; ew(0); return i;
    }
    if (!vk[t] && !lazy_below && lf_end == cn && barrier != cn && c_h > ctl[nctl - 1].h) {   /* сравнение f32 + переход: ложь при NaN сохраняется */
        u32 op = code[lf_start], k = (op & 0xFFFF) - OP_FCMP0, kk = k / NM, mode = k % NM;
        code[lf_start] = (OP_FCMPB0 + kk * 2 * NM + (nz ? 0 : NM) + mode) | (op & 0xFFFF0000u);
        u32 i = cn; ew(0); return i;
    }
    lz_flush_below_top();
    if (vk[t] == 1) { ew((nz ? OP_BRNZ_L : OP_BRZ_L) | vv[t] << 16); u32 i = cn; ew(0); return i; }
    if (vk[t] == 2) mat(t);
    ew(nz ? OP_BRNZ_S : OP_BRZ_S); u32 i = cn; ew(0); return i;
}
/* Безусловный переход на метку глубины d (значения уже на стеке, c_h - текущая высота). */
static int emit_br(u32 d) {
    ctl_t* f = &ctl[nctl - 1 - (int)d];
    if (f->kind == 3) { ew(OP_RET); return 0; }
    u32 keep = f->kind == 1 ? 0 : f->ar;
    if (c_h < f->h + keep) { trap("invalid module: stack height at branch"); return -1; }
    u32 drop = c_h - keep - f->h, i;
    if (!drop) { ew(OP_JMP); i = cn; ew(0); }
    else { ew(OP_BRD); i = cn; ew(0); ew(keep << 16 | drop); }
    if (f->kind == 1) code[i] = f->start; else ref_fwd(f, i);
    return 0;
}
static int flag_ok(u8 op) {                                        /* обработчик команды умеет сливаться с local.tee/set */
    return !((op >= 0x6D && op <= 0x70) || (op >= 0x7F && op <= 0x82) || (op >= 0xA8 && op <= 0xAB) || (op >= 0xAE && op <= 0xB1) || op == 0x3F || op == 0x40);
}
static int gen_effect(u32 op, int* pop, int* push) {              /* общие команды: сколько снимают и кладут */
    *push = 1;
    if (op == 0x45 || op == 0x50 || (op >= 0x67 && op <= 0x69) || (op >= 0x79 && op <= 0x7B) || (op >= 0x8B && op <= 0x91) ||
        (op >= 0x99 && op <= 0x9F) || (op >= 0xA7 && op <= 0xC4)) { *pop = 1; return 1; }
    if ((op >= 0x46 && op <= 0x4F) || (op >= 0x51 && op <= 0x66) || (op >= 0x6A && op <= 0x78) || (op >= 0x7C && op <= 0x8A) ||
        (op >= 0x92 && op <= 0x98) || (op >= 0xA0 && op <= 0xA6)) { *pop = 2; return 1; }
    return 0;
}
/* Двуместная команда семейства id0 (режим SS): режим выбирается по видам двух верхних элементов виртуального стека. */
static void emit_bin(u32 id0) {
    u32 ia = c_h - 2, ib = c_h - 1, mode, imm = 0, w1 = 0; int has1 = 0;
    if (vk[ia] == 2 && vk[ib]) mat(ia);                           /* константа слева при ленивом правом: слева - на стек */
    u8 ka = vk[ia], kb = vk[ib];
    if (!ka) {
        if (!kb) mode = M_SS;
        else if (kb == 1) { mode = M_SL; imm = vv[ib]; }
        else { mode = M_SK; w1 = vv[ib]; has1 = 1; }
    } else if (ka == 1) {
        imm = vv[ia];
        if (!kb) mode = M_LS;
        else { mode = kb == 1 ? M_LL : M_LK; w1 = vv[ib]; has1 = 1; }
    } else { mode = M_KS; w1 = vv[ia]; has1 = 1; }
    lp_start = cn;
    ew((id0 + mode) | imm << 16);
    if (has1) ew(w1);
    lp_end = cn; c_h -= 2; hpush(1);
}
#define CERR(m) do { trap(m); return -1; } while (0)
#define NEED(n) do { if (c_h < ctl[nctl - 1].h + (u32)(n)) CERR("invalid module: stack underflow"); } while (0)

static int compile_func(func_t* f) {
    const u8* p = f->code; const u8* end = f->end;
    const ftype_t* ft = &types[f->type];
    u32 nloc = ft->np + f->nlocals;
    f->centry = cn; nctl = 0; c_h = c_maxh = 0; c_unr = 0; c_inv = 0; barrier = cn; lc_start = lc_end = lf_start = lf_end = lp_start = lp_end = ~0u;
    ctl[0].kind = 3; ctl[0].ar = ft->nr; ctl[0].dead = 0; ctl[0].h = 0; ctl[0].start = 0; ctl[0].pend = 0; ctl[0].ifz = 0; nctl = 1;
    while (p < end && !trapped) {
        u8 op = *p++;
        if (c_unr && op != 0x02 && op != 0x03 && op != 0x04 && op != 0x05 && op != 0x0B) { skip_imm(op, &p, end, nloc); continue; }
        switch (op) {
        case 0x00: lz_flush(); ew(OP_TRAP); ew(TM_UNREACH); c_unr = 1; break;
        case 0x01: break;
        case 0x02: case 0x03: case 0x04: {
            u8 bt = *p; u32 ar = 0; int bad = 0;
            if (bt == 0x40) p++;
            else if (bt == 0x7F || bt == 0x7E || bt == 0x7D || bt == 0x7C) { p++; ar = 1; }
            else { i64 ti = leb_s(&p, end, 64); if (ti < 0 || (u64)ti >= ntypes || types[ti].np) bad = 1; else ar = types[ti].nr; }
            if (nctl >= 256) CERR("blocks nested too deep");
            ctl_t* n = &ctl[nctl];
            n->kind = op == 0x03 ? 1 : op == 0x04 ? 2 : 0; n->ar = (u8)ar; n->pend = 0; n->ifz = 0; n->start = 0;
            if (c_unr) { n->dead = 1; n->h = c_h; nctl++; break; }       /* вход в недостижимом коде: тело пропускается */
            if (bad) { lz_flush(); ew(OP_TRAP); ew(TM_BLOCKP); n->dead = 1; n->h = c_h; nctl++; c_unr = 1; break; }
            n->dead = 0;
            if (op == 0x04) {
                NEED(1);
                u32 i = cond_branch(0);                           /* ноль - к else или концу */
                c_h--; n->ifz = i + 1;
            } else lz_flush();
            n->h = c_h;
            if (op == 0x03) { n->start = cn; barrier = cn; }
            nctl++;
            break;
        }
        case 0x05: {
            ctl_t* fr = &ctl[nctl - 1];
            if (fr->kind != 2) CERR("else without if");
            if (!c_unr) {
                if (c_h != fr->h + fr->ar) CERR("invalid module: stack height at else");
                lz_flush(); ew(OP_JMP); u32 i = cn; ew(0); ref_fwd(fr, i);
            }
            if (fr->ifz) { code[fr->ifz - 1] = cn; fr->ifz = 0; }
            barrier = cn; c_h = fr->h; c_unr = fr->dead;
            fr->kind = 0;                                         /* дальше - как блок: второй else недопустим */
            break;
        }
        case 0x0B: {
            ctl_t* fr = &ctl[nctl - 1];
            if (nctl == 1) {                                      /* конец функции */
                if (!c_unr) {
                    if (c_h != ft->nr) CERR("invalid module: stack height at function end");
                    lz_flush(); ew(OP_RET);
                }
                f->maxh = c_maxh;
                return trapped ? -1 : 0;
            }
            if (!c_unr) {
                if (c_h != fr->h + fr->ar) CERR("invalid module: stack height at end");
                lz_flush();
            }
            if (fr->pend || fr->ifz) barrier = cn;
            patch_chain(fr->pend, cn);
            if (fr->ifz) code[fr->ifz - 1] = cn;
            nctl--;
            c_unr = fr->dead; c_h = fr->h; hpush(fr->ar);
            break;
        }
        case 0x0C: {
            u32 d = leb_u(&p, end); if (d >= (u32)nctl) CERR("bad branch depth");
            lz_flush();
            if (emit_br(d)) return -1;
            c_unr = 1; break;
        }
        case 0x0D: {
            u32 d = leb_u(&p, end); if (d >= (u32)nctl) CERR("bad branch depth");
            NEED(1);
            ctl_t* tf = &ctl[nctl - 1 - (int)d];
            u32 keep = tf->kind == 1 ? 0 : tf->ar;
            if (tf->kind != 3 && c_h - 1 >= tf->h + keep && c_h - 1 - keep - tf->h == 0) {    /* без переноса значений - прямой переход */
                u32 i = cond_branch(1); c_h--;
                if (tf->kind == 1) code[i] = tf->start; else ref_fwd(tf, i);
            } else {
                u32 skip = cond_branch(0); c_h--;                 /* не взято - мимо перехода */
                if (emit_br(d)) return -1;
                code[skip] = cn; barrier = cn;
            }
            break;
        }
        case 0x0E: {
            u32 cnt = leb_u(&p, end);
            if (cnt > (u32)(end - p)) CERR("bad br_table");
            NEED(1); lz_flush(); c_h--;
            ew(OP_BRTAB); ew(cnt);
            for (u32 i = 0; i <= cnt && !trapped; i++) {
                u32 d = leb_u(&p, end); if (d >= (u32)nctl) CERR("bad branch depth");
                ctl_t* tf = &ctl[nctl - 1 - (int)d];
                u32 keep = tf->kind == 1 ? 0 : tf->ar, at = cn;
                if (tf->kind == 3) { ew(0); ew(0xFFFFFFFFu); continue; }
                if (c_h < tf->h + keep) CERR("invalid module: stack height at br_table");
                ew(0); ew(keep << 16 | (c_h - keep - tf->h));
                if (tf->kind == 1) code[at] = tf->start; else ref_fwd(tf, at);
            }
            c_unr = 1; break;
        }
        case 0x0F: NEED(ft->nr); lz_flush(); ew(OP_RET); c_unr = 1; break;
        case 0x10: {
            u32 fi = leb_u(&p, end); if (fi >= nfuncs) CERR("call: bad function index");
            const ftype_t* t = &types[funcs[fi].type];
            NEED(t->np); lz_flush(); ew(OP_CALL | fi << 16);
            c_h -= t->np; hpush(t->nr); break;
        }
        case 0x11: {
            u32 ti = leb_u(&p, end);
            if (trapped || ti >= ntypes) CERR("call_indirect: bad type");
            if (p >= end || *p++ != 0) CERR("unsupported call_indirect table index");
            if (!table) CERR("call_indirect without table");
            const ftype_t* t = &types[ti];
            NEED(t->np + 1u); lz_flush(); ew(OP_CALLI | ti << 16);
            c_h -= t->np + 1u; hpush(t->nr); break;
        }
        case 0x1A:
            NEED(1);
            if (!vk[c_h - 1]) ew(0x1A);
            c_h--; break;
        case 0x1B: case 0x1C: {
            if (op == 0x1C) {
                u32 k = leb_u(&p, end);
                if (trapped || k != 1 || p >= end || !wasm_valtype(*p))
                    CERR("unsupported typed select result type");
                p++;
            }
            NEED(3);
            u32 ic = c_h - 1, ib = c_h - 2, ia = c_h - 3;
            if (vk[ic] != 2) {                                    /* значения и условие: со стека, локальная или константа; константа-условие - на стек */
                lp_start = cn;
                ew(OP_SEL | (u32)(vk[ia] | vk[ib] << 2 | vk[ic] << 4) << 16);
                if (vk[ia]) ew(vv[ia]);
                if (vk[ib]) ew(vv[ib]);
                if (vk[ic]) ew(vv[ic]);
            } else { lz_flush(); lp_start = cn; ew(0x1B); }
            lp_end = cn; c_h -= 2; vk[c_h - 1] = 0; break;
        }
        case 0x20: {
            u32 i = leb_u(&p, end); if (i >= nloc) CERR("bad local");
            lz_push(1, i); break;
        }
        case 0x21: case 0x22: {
            u32 i = leb_u(&p, end); if (i >= nloc) CERR("bad local");
            NEED(1);
            u32 t = c_h - 1;
            for (u32 j = ctl[nctl - 1].h; j < t; j++) if (vk[j] == 1 && vv[j] == i) mat(j);   /* запись в i не должна менять отложенное чтение i */
            if (!vk[t] && lp_end == cn && barrier != cn && !(code[lp_start] & 0xC000u)) {      /* результат команды - сразу в локальную */
                code[lp_start] |= op == 0x21 ? 0xC000u : 0x8000u;
                ew(i);
            } else if (vk[t]) { ew((vk[t] == 1 ? OP_SETL : OP_SETK) | i << 16); ew(vv[t]); }
            else ew((op == 0x21 ? OP_SET : OP_TEE) | i << 16);
            if (op == 0x21) c_h--;
            break;
        }
        case 0x23: case 0x24: {
            u32 i = leb_u(&p, end);
            if (i >= nglobals) { lz_flush(); ew(OP_TRAP); ew(TM_GLOBAL); c_unr = 1; break; }
            if (op == 0x24 && !global_mut[i]) CERR("global.set on immutable global");
            if (op == 0x23) { lz_flush(); ew(OP_GGET | i << 16); hpush(1); }
            else { NEED(1); lz_flush(); ew(OP_GSET | i << 16); c_h--; }
            break;
        }
        case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: {
            if (!has_memory) CERR("memory instruction without memory");
            u32 align = leb_u(&p, end); u32 off = leb_u(&p, end);
            static const u8 max_align[14] = { 2,3,2,3,0,0,1,1,0,0,1,1,2,2 };
            if (trapped || align > max_align[op - 0x28]) CERR("invalid WebAssembly load alignment");
            NEED(1);
            static const u8 ldk[8] = { 0, 1, 0, 1, 3, 2, 5, 4 };    /* 0x28..0x2F -> LD32, LD64, LD32, LD64, LD8S, LD8U, LD16S, LD16U */
            if (op <= 0x2F) {
                u32 k = ldk[op - 0x28];
                u32 id = LD32_S + k * 2;
                u32 t = c_h - 1;
                if (vk[t] == 1) { lp_start = cn; ew((id + 1) | vv[t] << 16); ew(off); vk[t] = 0; }
                else { if (vk[t] == 2) mat(t); lp_start = cn; ew(id); ew(off); }
                lp_end = cn;
            } else { lz_flush(); ew(op); ew(off); }
            break;
        }
        case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: {
            if (!has_memory) CERR("memory instruction without memory");
            u32 align = leb_u(&p, end); u32 off = leb_u(&p, end);
            static const u8 max_align[9] = { 2,3,2,3,0,1,0,1,2 };
            if (trapped || align > max_align[op - 0x36]) CERR("invalid WebAssembly store alignment");
            NEED(2);
            static const u8 stk[9] = { 0, 1, 0, 1, 2, 3, 2, 3, 0 };   /* 0x36..0x3E -> ST32, ST64, ST8, ST16 */
            u32 id = ST32_SS + stk[op - 0x36] * 3;
            u32 iv = c_h - 1, ia = c_h - 2;
            if (vk[iv] == 1) {
                if (vk[ia] == 1) { ew((id + 2) | vv[ia] << 16); ew(vv[iv]); ew(off); }               /* LL */
                else { if (vk[ia] == 2) mat(ia); ew((id + 1) | vv[iv] << 16); ew(off); }             /* SL */
            } else {
                if (vk[ia]) mat(ia);
                if (vk[iv]) mat(iv);
                ew(id); ew(off);
            }
            c_h -= 2; break;
        }
        case 0x3F: case 0x40:
            if (!has_memory) CERR("memory size/grow without memory");
            if (p >= end || *p++ != 0) CERR("unsupported memory index immediate");
            lz_flush();
            if (op == 0x3F) { ew(op); hpush(1); } else { NEED(1); ew(op); }
            break;
        case 0x41: lz_push(2, (u32)leb_s(&p, end, 32)); break;
        case 0x43: {
            if (p + 4 > end) CERR("truncated f32.const");
            lz_push(2, (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24); p += 4; break;
        }
        case 0x42: case 0x44: {
            u64 v = 0;
            if (op == 0x42) v = (u64)leb_s(&p, end, 64);
            else { if (p + 8 > end) CERR("truncated f64.const"); for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; p += 8; }
            lz_flush(); ew(op); ew((u32)v); ew((u32)(v >> 32)); hpush(1); break;
        }
        case 0xFC: {
            u32 sub = leb_u(&p, end);
            if (sub <= 7) { NEED(1); lz_flush(); ew(OP_FC + sub); }
            else if (sub == 10) {
                if (!has_memory) CERR("memory.copy without memory");
                u32 dst_memory = leb_u(&p, end), src_memory = leb_u(&p, end);
                if (trapped || dst_memory || src_memory) CERR("unsupported memory.copy memory index");
                NEED(3); lz_flush(); ew(OP_FC + sub); c_h -= 3;
            }
            else if (sub == 11) {
                if (!has_memory) CERR("memory.fill without memory");
                u32 memory_index = leb_u(&p, end);
                if (trapped || memory_index) CERR("unsupported memory.fill memory index");
                NEED(3); lz_flush(); ew(OP_FC + sub); c_h -= 3;
            }
            else { p--; skip_imm(0xFC, &p, end); if (sub == 8 || sub == 9) { lz_flush(); ew(OP_TRAP); ew(TM_PASSIVE); c_unr = 1; } }
            break;
        }
        default: {
            int pops = 0, pushes = 0, k = -1;
            if (op >= 0x46 && op <= 0x4F) { NEED(2); emit_bin(OP_CMP0 + (u32)(op - 0x46) * NM); lc_start = lp_start; lc_end = cn; break; }
            for (int i = 0; i < 9; i++) if (i32_arith_wasm[i] == op) k = i;
            if (k >= 0) { NEED(2); emit_bin(IADD_SS + (u32)k * NM); break; }
            if (op >= 0x92 && op <= 0x95) { NEED(2); emit_bin(FADD_SS + (u32)(op - 0x92) * NM); break; }
            if (op >= 0x5B && op <= 0x60) { NEED(2); emit_bin(FEQ_SS + (u32)(op - 0x5B) * NM); lf_start = lp_start; lf_end = cn; break; }
            if (op == 0x45 && !fusable_cmp() && p < end && (*p == 0x0D || *p == 0x04)) { NEED(1); c_inv ^= 1; break; }   /* eqz перед br_if/if - обратное условие */
            if (op == 0x45 && fusable_cmp()) {                    /* eqz от сравнения - обратное сравнение */
                u32 w = code[lc_start], kk = ((w & 0xFFFF) - OP_CMP0) / NM, mode = ((w & 0xFFFF) - OP_CMP0) % NM;
                code[lc_start] = (OP_CMP0 + cmp_inv[kk] * NM + mode) | (w & 0xFFFF0000u);
                break;
            }
            k = -1;
            for (int i = 0; i < 8; i++) if (ul_wasm[i] == op) k = i;
            if (k >= 0 && c_h > ctl[nctl - 1].h && vk[c_h - 1] == 1) { u32 t = c_h - 1; lp_start = cn; ew((UEQZ + (u32)k) | vv[t] << 16); lp_end = cn; vk[t] = 0; break; }
            if (gen_effect(op, &pops, &pushes)) {
                NEED(pops);
                lz_flush(); lp_start = cn; ew(op); lp_end = cn; c_h -= (u32)pops; hpush((u32)pushes);
                if (!flag_ok(op)) lp_end = ~0u;
                break;
            }
            lz_flush(); ew(OP_TRAP); ew(TM_UNKNOWN); c_unr = 1;
            break;
        }
        }
    }
    if (!trapped) trap("function without end");
    return -1;
}

/* ---------- загрузка ---------- */
static int load(void) {
    if (modlen < 8 || mod[0] || mod[1] != 'a' || mod[2] != 's' || mod[3] != 'm' ||
        mod[4] != 1 || mod[5] || mod[6] || mod[7]) {
        trap("not a WebAssembly 1.0 module"); return -1;
    }
    types = (ftype_t*)alloc(sizeof(ftype_t) * 256); funcs = (func_t*)alloc(sizeof(func_t) * MAXF);
    if (!types || !funcs) { trap("no memory"); return -1; }
    const u8* p = mod + 8; const u8* end = mod + modlen;
    u32 nfuncdecl = 0;
    u32 seen_sections = 0;
    has_memory = 0;
    u32 mem_max_set = 0;
    u32 has_start = 0;
    u8 last_section = 0;
    while (p < end && !trapped) {
        u8 id = *p++;
        u32 sz = leb_u(&p, end);
        if (trapped) return -1;
        if (id != 0) {
            if (id > 11) { trap("unsupported WebAssembly section id"); return -1; }
            if (seen_sections & (1u << id)) { trap("duplicate WebAssembly section"); return -1; }
            if (id < last_section) { trap("WebAssembly sections out of order"); return -1; }
            seen_sections |= 1u << id;
            last_section = id;
        }
        /* Check the declared length before forming p + sz: even pointer arithmetic
           beyond one-past-end is undefined in C, before any section read occurs. */
        if ((u64)sz > (u64)(end - p)) { trap("section past end"); return -1; }
        const u8* s = p;
        const u8* se = p + sz;
        if (id == 0) {                                            /* custom section: mandatory UTF-8 name prefix */
            u32 name_len = leb_u(&s, se);
            if (trapped || name_len > (u32)(se - s)) {
                trap("truncated custom section name");
                return -1;
            }
            /* Custom payload after the name is opaque, but the name itself is
               part of the WebAssembly envelope and must be present in-bounds. */
            if (!wasm_utf8_valid(s, name_len)) {
                trap("invalid UTF-8 custom section name");
                return -1;
            }
            s += name_len;
            s = se;
        } else if (id == 1) {                                     /* типы */
            u32 n = leb_u(&s, se);
            if (trapped || n > 256u - ntypes) { trap("too many types or truncated type section"); return -1; }
            for (u32 i = 0; i < n && !trapped; i++) {
                if (s >= se || *s++ != 0x60) { trap("bad type"); return -1; }
                ftype_t* t = &types[ntypes++];
                u32 np = leb_u(&s, se);
                if (trapped || np > 32 || np > (u32)(se - s)) { trap("bad type parameter vector"); return -1; }
                t->np = (u8)np;
                for (u32 k = 0; k < np; k++) {
                    t->p[k] = *s++;
                    if (!wasm_valtype(t->p[k])) { trap("unsupported parameter value type"); return -1; }
                }
                u32 nr = leb_u(&s, se);
                if (trapped || nr > 1 || nr > (u32)(se - s)) { trap("multi-value or truncated result vector"); return -1; }
                t->nr = (u8)nr;
                for (u32 k = 0; k < nr; k++) {
                    u8 rt = *s++;
                    if (!wasm_valtype(rt)) { trap("unsupported result value type"); return -1; }
                    t->r = rt;
                }
            }
        } else if (id == 2) {                                     /* импорт */
            u32 n = leb_u(&s, se);
            if (trapped) return -1;
            for (u32 i = 0; i < n && !trapped; i++) {
                u32 ml = leb_u(&s, se);
                if (trapped || ml > (u32)(se - s)) { trap("truncated import module name"); return -1; }
                const u8* mn = s;
                if (!wasm_utf8_valid(mn, ml)) { trap("invalid UTF-8 import module name"); return -1; }
                s += ml;
                u32 nl = leb_u(&s, se);
                if (trapped || nl > (u32)(se - s)) { trap("truncated import field name"); return -1; }
                const u8* nn = s;
                if (!wasm_utf8_valid(nn, nl)) { trap("invalid UTF-8 import field name"); return -1; }
                s += nl;
                if (s >= se) { trap("truncated import descriptor"); return -1; }
                u8 kind = *s++;
                if (kind == 0) {
                    u32 ti = leb_u(&s, se);
                    if (trapped) return -1;
                    if (ti >= ntypes) { trap("bad imported function type"); return -1; }
                    int found = -1;
                    for (int k = 0; k < IMP_N; k++) if ((name_is(mn, ml, "kira") || name_is(mn, ml, "env")) && name_is(nn, nl, imp_names[k])) found = k;
                    if (found < 0) { trap("unknown import (module kira: write, sleep, ticks, exit, gfx_create, gfx_present, gfx_event)"); return -1; }
                    if (!import_type_ok((u32)found, &types[ti])) { trap("bad KIRA import signature"); return -1; }
                    if (nfuncs == MAXF) { trap("too many functions"); return -1; }
                    funcs[nfuncs].type = ti; funcs[nfuncs].import = 1; funcs[nfuncs].imp_id = (u32)found; nfuncs++; nimports++;
                } else if (kind == 2) {                           /* импорт памяти: создаём свою */
                    if (has_memory || s >= se) { trap("multiple or truncated memory imports"); return -1; }
                    u8 fl = *s++;
                    if (fl & ~1u) { trap("unsupported memory import limits"); return -1; }
                    u32 min_pages = leb_u(&s, se);
                    if (trapped) return -1;
                    u32 max_pages = MAXMEM / 65536u;
                    if (fl & 1u) {
                        max_pages = leb_u(&s, se);
                        if (trapped) return -1;
                    }
                    if (min_pages > max_pages || min_pages > MAXMEM / 65536u) {
                        trap("invalid or excessive imported memory limits"); return -1;
                    }
                    has_memory = 1;
                    mem_pages = min_pages;
                    mem_max = max_pages > MAXMEM / 65536u ? MAXMEM / 65536u : max_pages;
                    mem_max_set = (fl & 1u) != 0;
                } else { trap("only function and memory imports are supported"); return -1; }
            }
        } else if (id == 3) {                                     /* функции */
            nfuncdecl = leb_u(&s, se);
            if (trapped || nfuncdecl > MAXF - nfuncs) { trap("too many or truncated function declarations"); return -1; }
            for (u32 i = 0; i < nfuncdecl && !trapped; i++) {
                u32 ti = leb_u(&s, se);
                if (trapped || ti >= ntypes) { trap("bad function type"); return -1; }
                funcs[nfuncs + i].type = ti;
                funcs[nfuncs + i].import = 0;
            }
            if (trapped) return -1;
            nfuncs += nfuncdecl;
        } else if (id == 4) {                                     /* таблица */
            u32 n = leb_u(&s, se);
            if (trapped) return -1;
            if (n > 1) { trap("multiple WebAssembly tables are unsupported"); return -1; }
            if (n) {
                if ((u32)(se - s) < 2 || *s++ != 0x70) { trap("unsupported table element type"); return -1; }
                u8 fl = *s++;
                if (fl & ~1u) { trap("unsupported table limits"); return -1; }
                u32 min_table = leb_u(&s, se);
                if (trapped) return -1;
                u32 max_table = 65536u;
                if (fl & 1u) {
                    max_table = leb_u(&s, se);
                    if (trapped) return -1;
                }
                if (min_table > max_table || min_table > 65536u) { trap("invalid or excessive table limits"); return -1; }
                ntable = min_table;
                table = (u32*)alloc(4ull * (ntable + 1));
                if (!table) { trap("no memory for table"); return -1; }
                for (u32 i = 0; i < ntable; i++) table[i] = ~0u;
            }
        } else if (id == 5) {                                     /* память */
            u32 n = leb_u(&s, se);
            if (trapped) return -1;
            if (n > 1 || (n && has_memory)) { trap("multiple WebAssembly memories"); return -1; }
            if (n) {
                if (s >= se) { trap("truncated memory limits"); return -1; }
                u8 fl = *s++;
                if (fl & ~1u) { trap("unsupported memory limits"); return -1; }
                u32 min_pages = leb_u(&s, se);
                if (trapped) return -1;
                u32 max_pages = MAXMEM / 65536u;
                if (fl & 1u) {
                    max_pages = leb_u(&s, se);
                    if (trapped) return -1;
                }
                if (min_pages > max_pages || min_pages > MAXMEM / 65536u) {
                    trap("invalid or excessive memory limits"); return -1;
                }
                has_memory = 1;
                mem_pages = min_pages;
                mem_max = max_pages > MAXMEM / 65536u ? MAXMEM / 65536u : max_pages;
                mem_max_set = (fl & 1u) != 0;
            }
        } else if (id == 6) {                                     /* глобальные */
            u32 n = leb_u(&s, se);
            if (trapped || n > MAXG - nglobals) { trap("too many globals or truncated global section"); return -1; }
            for (u32 i = 0; i < n && !trapped; i++) {
                if ((u32)(se - s) < 2) { trap("truncated global type"); return -1; }
                u8 gt = *s++, mut = *s++;
                if (!wasm_valtype(gt) || mut > 1) { trap("invalid global type or mutability"); return -1; }
                u64 initial_value = const_expr(&s, se, gt);
                if (trapped) return -1;
                globals[nglobals] = initial_value;
                global_mut[nglobals] = mut;
                nglobals++;
            }
        } else if (id == 7) {                                     /* экспорт: точка входа */
            u32 n = leb_u(&s, se);
            if (trapped) return -1;
            for (u32 i = 0; i < n && !trapped; i++) {
                u32 nl = leb_u(&s, se);
                if (trapped || nl > (u32)(se - s)) { trap("truncated export name"); return -1; }
                const u8* nn = s;
                if (!wasm_utf8_valid(nn, nl)) { trap("invalid UTF-8 export name"); return -1; }
                s += nl;
                if (s >= se) { trap("truncated export descriptor"); return -1; }
                u8 kind = *s++;
                u32 idx = leb_u(&s, se);
                if (trapped) return -1;
                if ((kind == 0 && idx >= nfuncs) ||
                    (kind == 1 && (!table || idx != 0)) ||
                    (kind == 2 && (!has_memory || idx != 0)) ||
                    (kind == 3 && idx >= nglobals) || kind > 3) {
                    trap("export index out of range"); return -1;
                }
                if (kind == 0 && (name_is(nn, nl, "main") || name_is(nn, nl, "_start") || name_is(nn, nl, "kmain"))) entry = idx;
            }
        } else if (id == 8) {
            entry = leb_u(&s, se);
            if (trapped) return -1;
            has_start = 1;
        } else if (id == 9) {                                     /* элементы таблицы */
            u32 n = leb_u(&s, se);
            if (trapped || (n && !table)) { trap("element segment without a table"); return -1; }
            for (u32 i = 0; i < n && !trapped; i++) {
                u32 fl = leb_u(&s, se);
                if (trapped || fl != 0) { trap("only active element segments (flag 0) are supported"); return -1; }
                u64 off64 = const_expr(&s, se, 0x7F);
                u32 cnt = leb_u(&s, se);
                if (trapped || off64 > ntable || (u64)cnt > (u64)ntable - off64) {
                    trap("element segment outside table"); return -1;
                }
                u32 off = (u32)off64;
                for (u32 k = 0; k < cnt; k++) {
                    u32 f = leb_u(&s, se);
                    if (trapped || f >= nfuncs) { trap("element function index out of range"); return -1; }
                    table[off + k] = f;
                }
            }
        } else if (id == 10) {                                    /* код */
            u32 n = leb_u(&s, se);
            if (n != nfuncdecl) { trap("function and code counts differ"); return -1; }
            if (!has_memory) mem_max = 0;
            else if (!mem_max_set) mem_max = MAXMEM / 65536;
            ccap = 2 * sz + 8 * n + 16;                           /* не больше двух слов на байт кода + запас на функцию */
            code = (u32*)alloc(4ull * ccap);
            if (!code) { trap("no memory for compiled code"); return -1; }
            if (n != nfuncdecl || n > nfuncs - nimports) { trap("function and code counts differ"); return -1; }
            u32 first = nimports;                                /* объявления уже зарегистрированы до exports/elements */
            for (u32 i = 0; i < n && !trapped; i++) {
                u32 bs = leb_u(&s, se);
                if (trapped || bs > (u32)(se - s)) { trap("function body past code section"); return -1; }
                const u8* b = s; const u8* be = s + bs;
                u32 groups = leb_u(&b, be), nl = 0;
                if (trapped) return -1;
                for (u32 g = 0; g < groups; g++) {
                    u32 count = leb_u(&b, be);
                    if (trapped || b >= be || !wasm_valtype(*b) || count > 4096u - nl) {
                        trap("bad function local declarations"); return -1;
                    }
                    b++; nl += count;
                }
                func_t* f = &funcs[first + i];
                f->code = b; f->end = be; f->nlocals = nl;
                if (compile_func(f)) return -1;
                s = be;
            }
        } else if (id == 11) {                                    /* данные - после выделения памяти */
            u32 n = leb_u(&s, se);
            if (trapped || (n && !has_memory)) { trap("data segments without memory"); return -1; }
            if (n && !mem) {
                if (!mem_max_set) mem_max = has_memory ? MAXMEM / 65536 : 0;
                if (mem_max * 65536ull > MAXMEM) mem_max = MAXMEM / 65536;
                if (mem_pages > mem_max) { trap("module needs more memory than 2 MB"); return -1; }
                mem = (u8*)alloc((u64)(mem_max ? mem_max : 1) * 65536);
                if (!mem) { trap("no memory for the module"); return -1; }
            }
            for (u32 i = 0; i < n && !trapped; i++) {
                u32 fl = leb_u(&s, se);
                if (trapped || fl == 1) { trap("passive data segments are not supported"); return -1; }
                if (fl > 2) { trap("unsupported data segment flags"); return -1; }
                if (fl == 2 && leb_u(&s, se) != 0) { trap("unsupported data memory index"); return -1; }
                if (trapped) return -1;
                u64 off64 = const_expr(&s, se, 0x7F);
                u32 len = leb_u(&s, se);
                if (trapped || off64 > (u64)mem_pages * 65536 ||
                    (u64)len > (u64)mem_pages * 65536 - off64 ||
                    len > (u32)(se - s)) { trap("data segment out of memory or section"); return -1; }
                u32 off = (u32)off64;
                for (u32 k = 0; k < len; k++) mem[off + k] = s[k];
                s += len;
            }
        }
        if (id != 0 && !trapped && s != se) {
            trap("trailing bytes in WebAssembly section");
            return -1;
        }
        p = se;
    }
    if (!trapped && has_memory && !mem) {
        if (!mem_max_set) mem_max = MAXMEM / 65536;
        if (mem_max * 65536ull > MAXMEM) mem_max = MAXMEM / 65536;
        if (mem_pages > mem_max) { trap("module needs more memory than 2 MB"); return -1; }
        mem = (u8*)alloc((u64)(mem_max ? mem_max : 1) * 65536);
    }
    if (!trapped && entry == ~0u) trap("no exported main/_start");
    if (!trapped && entry >= nfuncs) trap("entry function index out of range");
    if (!trapped && entry < nfuncs) {
        const func_t* ef = &funcs[entry];
        const ftype_t* et = &types[ef->type];
        if (ef->import || et->np != 0 ||
            (has_start ? et->nr != 0 : (et->nr > 1 || (et->nr == 1 && et->r != 0x7F))))
            trap("invalid KIRA entry function signature");
    }
    if (!trapped && nfuncs > nimports && !code) trap("function and code counts differ");
    return trapped ? -1 : 0;
}

/* ---------- выполнение ---------- */
static u64* vs; static u32 sp;                   /* стек значений (sp - индекс вершины между вызовами execute) */
static int exit_req, exit_code;
static u32* fb; static u32 fbw, fbh;
static struct { const u32* pc; u64* fp; const func_t* f; } cs[DEPTH + 2];

static inline u32 ld16(const u8* p) { u16 v; __builtin_memcpy(&v, p, 2); return v; }
static inline u32 ld32(const u8* p) { u32 v; __builtin_memcpy(&v, p, 4); return v; }
static inline u64 ld64(const u8* p) { u64 v; __builtin_memcpy(&v, p, 8); return v; }
static inline void st16(u8* p, u16 v) { __builtin_memcpy(p, &v, 2); }
static inline void st32(u8* p, u32 v) { __builtin_memcpy(p, &v, 4); }
static inline void st64(u8* p, u64 v) { __builtin_memcpy(p, &v, 8); }
static u32 clz32(u32 x) { u32 n = 0; if (!x) return 32; while (!(x & 0x80000000u)) { x <<= 1; n++; } return n; }
static u32 ctz32(u32 x) { u32 n = 0; if (!x) return 32; while (!(x & 1)) { x >>= 1; n++; } return n; }
static u32 pop32(u32 x) { u32 n = 0; while (x) { n += x & 1; x >>= 1; } return n; }
static u64 clz64(u64 x) { u64 n = 0; if (!x) return 64; while (!(x >> 63)) { x <<= 1; n++; } return n; }
static u64 ctz64(u64 x) { u64 n = 0; if (!x) return 64; while (!(x & 1)) { x >>= 1; n++; } return n; }
static u64 pop64(u64 x) { u64 n = 0; while (x) { n += x & 1; x >>= 1; } return n; }

/* Импорт модуля "kira": аргументы снимаются со стека s, результат кладётся; возвращает новую вершину. */
static u64* host_call(u32 id, const ftype_t* t, u64* s) {
    u64 a[4] = { 0, 0, 0, 0 };
    for (int i = t->np - 1; i >= 0; i--) { u64 v = *--s; if (i < 4) a[i] = v; }
    u64 r = 0, lim = (u64)mem_pages * 65536;
    if (id == IMP_WRITE) {
        u32 p = (u32)a[0], n = (u32)a[1];
        if ((u64)p + n > lim) { trap("memory access out of bounds"); return s; }
        k_write((const char*)mem + p, n); r = n;
    }
    else if (id == IMP_SLEEP) k_sleep(a[0]);
    else if (id == IMP_TICKS) r = k_ticks();
    else if (id == IMP_EXIT) { exit_req = 1; exit_code = (int)a[0]; }
    else if (id == IMP_GFX_CREATE) { fbw = (u32)a[0]; fbh = (u32)a[1]; fb = (fbw && fbh && fbw <= 1024 && fbh <= 768) ? k_gfx_create(fbw, fbh) : 0; r = fb ? 0 : (u64)-1; }
    else if (id == IMP_GFX_PRESENT) {
        u32 p = (u32)a[0];
        if (!fb) r = (u64)-1;
        else {
            if ((u64)p + (u64)fbw * fbh * 4 > lim) { trap("memory access out of bounds"); return s; }
            u32 n = fbw * fbh, i = 0;
            for (; i + 1 < n; i += 2) st64((u8*)(fb + i), ld64(mem + p + 4 * i));           /* по 8 байт: кадр копируется вдвое быстрее */
            if (i < n) fb[i] = ld32(mem + p + 4 * i);
            k_gfx_present();
            k_yield();                                            /* кадр отдан композитору: пусть интерфейс успеет, даже если следующий кадр считается долго */
        }
    } else if (id == IMP_GFX_EVENT) {
        u32 p = (u32)a[0]; kevent_t e;
        if ((u64)p + 16 > lim) { trap("memory access out of bounds"); return s; }
        int got = k_gfx_event(&e);
        if (got > 0) { st32(mem + p, e.type); st32(mem + p + 4, e.a); st32(mem + p + 8, e.b); st32(mem + p + 12, e.c); }
        r = (u64)(i64)got;
    }
    if (t->nr) *s++ = r;
    return s;
}

/* Результат производящей команды: биты слова 15 (TF) - записать ещё и в локальную (номер - последнее слово команды),
   14 (PF) - снять результат со стека (local.tee / local.set, слитые с производящей командой). */
#ifdef WASM_FUEL                                                  /* только хост-тест с испорченными модулями: счётчик команд против зацикливания */
static u64 wasm_fuel;
#define FUEL_TICK do { if (!--wasm_fuel) { trap("fuel exhausted"); return; } } while (0)
#else
#define FUEL_TICK do { } while (0)
#endif
/* Кванты времени: раз в SLICE_TICKS переходов назад и вызовов проверяется часы; если процессор не отдавался дольше SLICE_NS,
   программа уступает его (k_yield) - окна и интерфейс не голодают, даже когда модуль считает кадр секундами. */
#ifdef WASM_FUEL
#define SLICE_TICKS 16                                            /* хост-тест с испорченными модулями: чаще, каждый переход назад стоит 64 единицы топлива */
#else
#define SLICE_TICKS 4096
#endif
#define SLICE_NS 4000000ull
static u64 slice_t0;
static int wasm_slice(void) {
#ifdef WASM_FUEL
    if (wasm_fuel <= SLICE_TICKS * 64u) { wasm_fuel = 0; trap("fuel exhausted"); return 1; }
    wasm_fuel -= SLICE_TICKS * 64u;
#endif
    u64 now = k_clock_ns();
    if (now - slice_t0 > SLICE_NS) { k_yield(); slice_t0 = k_clock_ns(); }
    return 0;
}
#define IBUD do { if (--ibud <= 0) { ibud = SLICE_TICKS; if (wasm_slice()) { sp = (u32)(s - vs); return; } } } while (0)
#define NEXT do { FUEL_TICK; w = *pc++; goto *tbl[w & 0x3FFF]; } while (0)    /* диспетчеризация в конце каждой команды (computed goto) */
#define FIN(r) if (w & 0xC000) { if (w & 0x8000) fp[*pc++] = (r); if (w & 0x4000) s--; } NEXT;
#define RAW_OPS(X) X(0x1A) X(0x1B) X(0x45) X(0x50) X(0x51) X(0x52) X(0x53) X(0x54) X(0x55) X(0x56) X(0x57) X(0x58) X(0x59) X(0x5A) X(0x61) X(0x62) X(0x63) X(0x64) X(0x65) X(0x66) X(0x67) X(0x68) X(0x69) X(0x6D) X(0x6E) X(0x6F) X(0x70) X(0x77) X(0x78) X(0x79) X(0x7A) X(0x7B) X(0x7C) X(0x7D) X(0x7E) X(0x7F) X(0x80) X(0x81) X(0x82) X(0x83) X(0x84) X(0x85) X(0x86) X(0x87) X(0x88) X(0x89) X(0x8A) X(0x8B) X(0x8C) X(0x8D) X(0x8E) X(0x8F) X(0x90) X(0x91) X(0x96) X(0x97) X(0x98) X(0x99) X(0x9A) X(0x9B) X(0x9C) X(0x9D) X(0x9E) X(0x9F) X(0xA0) X(0xA1) X(0xA2) X(0xA3) X(0xA4) X(0xA5) X(0xA6) X(0xA7) X(0xAC) X(0xAD) X(0xB2) X(0xB3) X(0xB4) X(0xB5) X(0xB6) X(0xB7) X(0xB8) X(0xB9) X(0xBA) X(0xBB) X(0xBC) X(0xBD) X(0xBE) X(0xBF) X(0xC0) X(0xC1) X(0xC2) X(0xC3) X(0xC4)
#define TBR(oc) [oc] = &&L_##oc,
#define TB5(n, e) [n##_SS] = &&L_##n##_SS, [n##_SL] = &&L_##n##_SL, [n##_SK] = &&L_##n##_SK, [n##_LL] = &&L_##n##_LL, [n##_LK] = &&L_##n##_LK, [n##_LS] = &&L_##n##_LS, [n##_KS] = &&L_##n##_KS,
#define TBB(n, e) [n##B_SS] = &&L_##n##B_SS, [n##B_SL] = &&L_##n##B_SL, [n##B_SK] = &&L_##n##B_SK, [n##B_LL] = &&L_##n##B_LL, [n##B_LK] = &&L_##n##B_LK, [n##B_LS] = &&L_##n##B_LS, [n##B_KS] = &&L_##n##B_KS,
#define TBL1(n, e) [n] = &&L_##n,
#define TBLD(n, s, e) [n##_S] = &&L_##n##_S, [n##_L] = &&L_##n##_L,
#define TBST(n, s, e) [n##_SS] = &&L_##n##_SS, [n##_SL] = &&L_##n##_SL, [n##_LL] = &&L_##n##_LL,
/* Макросы семейств: те же выражения, что при предкомпиляции (I32_ARITH и др.), по пять режимов операндов. */
#define CBI(n, e) \
    L_##n##_SS: { u32 b = (u32)s[-1], a = (u32)s[-2]; s--; u64 r = (u64)(e); s[-1] = r; FIN(r) } \
    L_##n##_SL: { u32 b = (u32)fp[w >> 16], a = (u32)s[-1]; u64 r = (u64)(e); s[-1] = r; FIN(r) } \
    L_##n##_SK: { u32 b = *pc++, a = (u32)s[-1]; u64 r = (u64)(e); s[-1] = r; FIN(r) } \
    L_##n##_LL: { u32 a = (u32)fp[w >> 16], b = (u32)fp[*pc++]; u64 r = (u64)(e); *s++ = r; FIN(r) } \
    L_##n##_LK: { u32 a = (u32)fp[w >> 16], b = *pc++; u64 r = (u64)(e); *s++ = r; FIN(r) } \
    L_##n##_LS: { u32 a = (u32)fp[w >> 16], b = (u32)s[-1]; u64 r = (u64)(e); s[-1] = r; FIN(r) } \
    L_##n##_KS: { u32 a = *pc++, b = (u32)s[-1]; u64 r = (u64)(e); s[-1] = r; FIN(r) }
#define CBF(n, e) \
    L_##n##_SS: { float b = gf(s[-1]), a = gf(s[-2]); s--; u64 r = (u64)(e); s[-1] = r; FIN(r) } \
    L_##n##_SL: { float b = gf(fp[w >> 16]), a = gf(s[-1]); u64 r = (u64)(e); s[-1] = r; FIN(r) } \
    L_##n##_SK: { float b = gf(*pc++), a = gf(s[-1]); u64 r = (u64)(e); s[-1] = r; FIN(r) } \
    L_##n##_LL: { float a = gf(fp[w >> 16]), b = gf(fp[*pc++]); u64 r = (u64)(e); *s++ = r; FIN(r) } \
    L_##n##_LK: { float a = gf(fp[w >> 16]), b = gf(*pc++); u64 r = (u64)(e); *s++ = r; FIN(r) } \
    L_##n##_LS: { float a = gf(fp[w >> 16]), b = gf(s[-1]); u64 r = (u64)(e); s[-1] = r; FIN(r) } \
    L_##n##_KS: { float a = gf(*pc++), b = gf(s[-1]); u64 r = (u64)(e); s[-1] = r; FIN(r) }
#define CBB(n, e) \
    L_##n##B_SS: { u32 b = (u32)s[-1], a = (u32)s[-2]; s -= 2; u32 t = *pc++; if (e) { pc = cb + t; IBUD; } NEXT; } \
    L_##n##B_SL: { u32 b = (u32)fp[w >> 16], a = (u32)s[-1]; s--; u32 t = *pc++; if (e) { pc = cb + t; IBUD; } NEXT; } \
    L_##n##B_SK: { u32 b = pc[0], a = (u32)s[-1], t = pc[1]; pc += 2; s--; if (e) { pc = cb + t; IBUD; } NEXT; } \
    L_##n##B_LL: { u32 a = (u32)fp[w >> 16], b = (u32)fp[pc[0]], t = pc[1]; pc += 2; if (e) { pc = cb + t; IBUD; } NEXT; } \
    L_##n##B_LK: { u32 a = (u32)fp[w >> 16], b = pc[0], t = pc[1]; pc += 2; if (e) { pc = cb + t; IBUD; } NEXT; } \
    L_##n##B_LS: { u32 a = (u32)fp[w >> 16], b = (u32)s[-1], t = pc[0]; pc++; s--; if (e) { pc = cb + t; IBUD; } NEXT; } \
    L_##n##B_KS: { u32 a = pc[0], b = (u32)s[-1], t = pc[1]; pc += 2; s--; if (e) { pc = cb + t; IBUD; } NEXT; }
#define ID(x) (x)
#define NOT(x) (!(x))
#define CFB1(n, e, sfx, T) \
    L_##n##sfx##_SS: { float b = gf(s[-1]), a = gf(s[-2]); s -= 2; u32 t = *pc++; if (T(e)) { pc = cb + t; IBUD; } NEXT; } \
    L_##n##sfx##_SL: { float b = gf(fp[w >> 16]), a = gf(s[-1]); s--; u32 t = *pc++; if (T(e)) { pc = cb + t; IBUD; } NEXT; } \
    L_##n##sfx##_SK: { float b = gf(pc[0]), a = gf(s[-1]); u32 t = pc[1]; pc += 2; s--; if (T(e)) { pc = cb + t; IBUD; } NEXT; } \
    L_##n##sfx##_LL: { float a = gf(fp[w >> 16]), b = gf(fp[pc[0]]); u32 t = pc[1]; pc += 2; if (T(e)) { pc = cb + t; IBUD; } NEXT; } \
    L_##n##sfx##_LK: { float a = gf(fp[w >> 16]), b = gf(pc[0]); u32 t = pc[1]; pc += 2; if (T(e)) { pc = cb + t; IBUD; } NEXT; } \
    L_##n##sfx##_LS: { float a = gf(fp[w >> 16]), b = gf(s[-1]); u32 t = pc[0]; pc++; s--; if (T(e)) { pc = cb + t; IBUD; } NEXT; } \
    L_##n##sfx##_KS: { float a = gf(pc[0]), b = gf(s[-1]); u32 t = pc[1]; pc += 2; s--; if (T(e)) { pc = cb + t; IBUD; } NEXT; }
#define CFB(n, e) CFB1(n, e, B, ID) CFB1(n, e, N, NOT)
#define TBF1(n, e) \
    [n##B_SS] = &&L_##n##B_SS, [n##B_SL] = &&L_##n##B_SL, [n##B_SK] = &&L_##n##B_SK, [n##B_LL] = &&L_##n##B_LL, [n##B_LK] = &&L_##n##B_LK, [n##B_LS] = &&L_##n##B_LS, [n##B_KS] = &&L_##n##B_KS, \
    [n##N_SS] = &&L_##n##N_SS, [n##N_SL] = &&L_##n##N_SL, [n##N_SK] = &&L_##n##N_SK, [n##N_LL] = &&L_##n##N_LL, [n##N_LK] = &&L_##n##N_LK, [n##N_LS] = &&L_##n##N_LS, [n##N_KS] = &&L_##n##N_KS,
#define CUL(n, e) L_##n: { u64 a = fp[w >> 16]; u64 r = (u64)(e); *s++ = r; FIN(r) }
#define CLD(n, sz, e) \
    L_##n##_S: { u64 ea = (u64)(u32)s[-1] + pc[0]; pc++; if (ea + sz > ml) goto oob; u64 r = (e); s[-1] = r; FIN(r) } \
    L_##n##_L: { u64 ea = (u64)(u32)fp[w >> 16] + pc[0]; pc++; if (ea + sz > ml) goto oob; u64 r = (e); *s++ = r; FIN(r) }
#define CST(n, sz, e) \
    L_##n##_SS: { u64 v = s[-1], ea = (u64)(u32)s[-2] + pc[0]; pc++; s -= 2; if (ea + sz > ml) goto oob; e; NEXT; } \
    L_##n##_SL: { u64 v = fp[w >> 16], ea = (u64)(u32)s[-1] + pc[0]; pc++; s--; if (ea + sz > ml) goto oob; e; NEXT; } \
    L_##n##_LL: { u64 v = fp[pc[0]], ea = (u64)(u32)fp[w >> 16] + pc[1]; pc += 2; if (ea + sz > ml) goto oob; e; NEXT; }
/* общие команды без особых режимов */
#define GB32(oc, e) case oc: L_##oc: { u32 b = (u32)s[-1], a = (u32)s[-2]; s--; u64 r = (u64)(e); s[-1] = r; FIN(r) }
#define GB64(oc, e) case oc: L_##oc: { u64 b = s[-1], a = s[-2]; s--; u64 r = (u64)(e); s[-1] = r; FIN(r) }
#define GBF(oc, e) case oc: L_##oc: { float b = gf(s[-1]), a = gf(s[-2]); s--; u64 r = (u64)(e); s[-1] = r; FIN(r) }
#define GBD(oc, e) case oc: L_##oc: { double b = gd(s[-1]), a = gd(s[-2]); s--; u64 r = (u64)(e); s[-1] = r; FIN(r) }
#define GU(oc, e) case oc: L_##oc: { u64 a = s[-1]; u64 r = (u64)(e); s[-1] = r; FIN(r) }
#define DIVCHK(zero) if (zero) { trap("integer divide by zero"); return; }

/* Выполнение функции fi0 (аргументы на стеке значений, вершина sp); результат остаётся на стеке. */
static void execute(u32 fi0) {
    const func_t* cur = &funcs[fi0]; const ftype_t* ct = &types[cur->type];
    if (cur->import) { sp = (u32)(host_call(cur->imp_id, ct, vs + sp) - vs); return; }
    u64* fp = vs + (sp - ct->np);
    u32 nl = ct->np + cur->nlocals;
    if ((u64)(fp - vs) + nl + cur->maxh + 1 > VSTK) { trap("value stack overflow"); return; }
    for (u32 i = ct->np; i < nl; i++) fp[i] = 0;
    u64* s = fp + nl;
    const u32* cb = code; const u32* pc = cb + cur->centry; int ibud = SLICE_TICKS;
    u8* const m = mem; u64 ml = (u64)mem_pages * 65536;
    int depth = 0; u32 fi = 0;
    static const void* tbl[OP_END] = {                            /* адреса обработчиков по коду; остальное - в L_SLOW */
        [OP_PUSHL] = &&L_OP_PUSHL, [OP_PUSHK] = &&L_OP_PUSHK, [OP_SET] = &&L_OP_SET, [OP_SETL] = &&L_OP_SETL, [OP_SETK] = &&L_OP_SETK,
        [OP_TEE] = &&L_OP_TEE, [OP_GGET] = &&L_OP_GGET, [OP_GSET] = &&L_OP_GSET, [OP_JMP] = &&L_OP_JMP, [OP_BRZ_S] = &&L_OP_BRZ_S,
        [OP_BRNZ_S] = &&L_OP_BRNZ_S, [OP_BRZ_L] = &&L_OP_BRZ_L, [OP_BRNZ_L] = &&L_OP_BRNZ_L, [OP_BRD] = &&L_OP_BRD, [OP_INSL] = &&L_OP_INSL, [OP_INSK] = &&L_OP_INSK, [OP_SEL] = &&L_OP_SEL,
        I32_ARITH(TB5) I32_CMP(TB5) I32_CMP(TBB) F32_ARITH(TB5) F32_CMP(TB5) F32_CMP(TBF1) UL_LIST(TBL1) LD_LIST(TBLD) ST_LIST(TBST) RAW_OPS(TBR)
        [0xA8] = &&L_TRUNC, [0xA9] = &&L_TRUNC, [0xAA] = &&L_TRUNC, [0xAB] = &&L_TRUNC, [0xAE] = &&L_TRUNC, [0xAF] = &&L_TRUNC, [0xB0] = &&L_TRUNC, [0xB1] = &&L_TRUNC,
    };
    u32 w;
    if (!tbl[0]) for (u32 i = 0; i < OP_END; i++) if (!tbl[i]) tbl[i] = &&L_SLOW;
    for (;;) {
        FUEL_TICK; w = *pc++;
        goto *tbl[w & 0x3FFF];
        L_OP_PUSHL: *s++ = fp[w >> 16]; NEXT;
        L_OP_PUSHK: *s++ = (u64)*pc++; NEXT;
        L_OP_SET: fp[w >> 16] = *--s; NEXT;
        L_OP_SETL: fp[w >> 16] = fp[*pc++]; NEXT;
        L_OP_SETK: fp[w >> 16] = (u64)*pc++; NEXT;
        L_OP_TEE: fp[w >> 16] = s[-1]; NEXT;
        L_OP_GGET: *s++ = globals[w >> 16]; NEXT;
        L_OP_GSET: globals[w >> 16] = *--s; NEXT;
        L_OP_JMP: pc = cb + pc[0]; IBUD; NEXT;
        L_OP_BRZ_S: { u32 t = *pc++; if (!(u32)*--s) { pc = cb + t; IBUD; } NEXT; }
        L_OP_BRNZ_S: { u32 t = *pc++; if ((u32)*--s) { pc = cb + t; IBUD; } NEXT; }
        L_OP_BRZ_L: { u32 t = *pc++; if (!(u32)fp[w >> 16]) { pc = cb + t; IBUD; } NEXT; }
        L_OP_BRNZ_L: { u32 t = *pc++; if ((u32)fp[w >> 16]) { pc = cb + t; IBUD; } NEXT; }
        L_OP_INSL: { u32 d = *pc++; u64 v = fp[w >> 16]; for (u32 j = 0; j < d; j++) s[-(int)j] = s[-(int)j - 1]; s[-(int)d] = v; s++; NEXT; }
    L_OP_INSK: { u64 v = pc[0]; u32 d = pc[1]; pc += 2; for (u32 j = 0; j < d; j++) s[-(int)j] = s[-(int)j - 1]; s[-(int)d] = v; s++; NEXT; }
    L_OP_SEL: {                                                   /* виды операндов в старших битах: a - биты 0-1, b - 2-3, условие - 4-5 (0 стек, 1 локальная, 2 константа) */
        u32 ka = (w >> 16) & 3, kb = (w >> 18) & 3, kc = (w >> 20) & 3, wa = 0, wb = 0, wc = 0;
        if (ka) wa = *pc++;
        if (kb) wb = *pc++;
        if (kc) wc = *pc++;
        u64 cv = kc == 0 ? *--s : fp[wc];
        u64 bv = kb == 0 ? *--s : kb == 1 ? fp[wb] : wb;
        u64 av = ka == 0 ? *--s : ka == 1 ? fp[wa] : wa;
        u64 r = (u32)cv ? av : bv; *s++ = r; FIN(r)
    }
    L_OP_BRD: {                                            /* переход со снятием drop значений под keep результатами */
            u32 aux = pc[1], drop = aux & 0xFFFF;
            if (aux >> 16) s[-1 - (int)drop] = s[-1];
            s -= drop; pc = cb + pc[0]; IBUD; NEXT;
        }
        I32_ARITH(CBI) I32_CMP(CBI) I32_CMP(CBB) F32_ARITH(CBF) F32_CMP(CBF) F32_CMP(CFB)
        UL_LIST(CUL)
        LD_LIST(CLD)
        ST_LIST(CST)
    L_SLOW: {
        u32 op = w & 0x3FFF;
        switch (op) {
        case OP_BRTAB: {
            u32 cnt = pc[0], idx = (u32)*--s;
            if (idx > cnt) idx = cnt;
            const u32* e = pc + 1 + 2 * idx; u32 aux = e[1];
            if (aux == 0xFFFFFFFFu) goto do_ret;
            if (aux >> 16) s[-1 - (int)(aux & 0xFFFF)] = s[-1];
            s -= aux & 0xFFFF; pc = cb + e[0]; IBUD; continue;
        }
        case OP_RET:
        do_ret: {
            u32 nr = types[cur->type].nr;
            if (nr) fp[0] = s[-1];
            s = fp + nr;
            if (!depth) { sp = (u32)(s - vs); return; }
            depth--; pc = cs[depth].pc; fp = cs[depth].fp; cur = cs[depth].f;
            continue;
        }
        case OP_CALLI: {
            u32 ti = w >> 16, i = (u32)*--s;
            if (i >= ntable || table[i] == ~0u) { trap("call_indirect: no such table entry"); return; }
            fi = table[i];
            if (fi >= nfuncs) { trap("call_indirect: bad function"); return; }
            const ftype_t* a = &types[funcs[fi].type]; const ftype_t* b = &types[ti];
            if (!wasm_type_equal(a, b)) { trap("call_indirect: signature mismatch"); return; }
            goto do_call;
        }
        case OP_CALL: fi = w >> 16;
        do_call: {
            const func_t* c = &funcs[fi]; const ftype_t* t = &types[c->type];
            if (c->import) {
                s = host_call(c->imp_id, t, s);
                if (trapped || exit_req) { sp = (u32)(s - vs); return; }
                ml = (u64)mem_pages * 65536;
                continue;
            }
            if (depth >= DEPTH) { trap("call stack exhausted"); return; }
            u64* nfp = s - t->np; u32 n2 = t->np + c->nlocals;
            if ((u64)(nfp - vs) + n2 + c->maxh + 1 > VSTK) { trap("value stack overflow"); return; }
            cs[depth].pc = pc; cs[depth].fp = fp; cs[depth].f = cur; depth++;
            for (u32 i = t->np; i < n2; i++) nfp[i] = 0;
            fp = nfp; s = fp + n2; pc = cb + c->centry; cur = c; IBUD;
            continue;
        }
        case OP_TRAP: trap(trap_msgs[pc[0]]); return;
        case 0x1A: L_0x1A: s--; NEXT;
        case 0x1B: L_0x1B: { u32 c = (u32)s[-1]; u64 b = s[-2], a = s[-3]; s -= 2; u64 r = c ? a : b; s[-1] = r; FIN(r) }
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: {
            static const u8 sz[6] = { 1, 1, 2, 2, 4, 4 };
            u64 ea = (u64)(u32)s[-1] + pc[0]; pc++; u32 n = sz[op - 0x30];
            if (ea + n > ml) goto oob;
            u64 v = n == 1 ? m[ea] : n == 2 ? ld16(m + ea) : ld32(m + ea);
            if (op == 0x30) v = (u64)(i64)(signed char)v; else if (op == 0x32) v = (u64)(i64)(short)v; else if (op == 0x34) v = (u64)(i64)(int)v;
            s[-1] = v; continue;
        }
        case 0x3F: *s++ = mem_pages; continue;
        case 0x40: {
            u32 d = (u32)s[-1], old = mem_pages;
            if ((u64)mem_pages + d > mem_max) s[-1] = 0xFFFFFFFFu; else { mem_pages += d; s[-1] = old; ml = (u64)mem_pages * 65536; }
            continue;
        }
        case 0x42: case 0x44: *s++ = (u64)pc[0] | (u64)pc[1] << 32; pc += 2; continue;
        case 0x45: L_0x45: { u64 r = (u32)s[-1] == 0; s[-1] = r; FIN(r) }
        case 0x50: L_0x50: { u64 r = s[-1] == 0; s[-1] = r; FIN(r) }
        GB64(0x51, a == b) GB64(0x52, a != b) GB64(0x53, (i64)a < (i64)b) GB64(0x54, a < b) GB64(0x55, (i64)a > (i64)b)
        GB64(0x56, a > b) GB64(0x57, (i64)a <= (i64)b) GB64(0x58, a <= b) GB64(0x59, (i64)a >= (i64)b) GB64(0x5A, a >= b)
        GBD(0x61, a == b) GBD(0x62, a != b) GBD(0x63, a < b) GBD(0x64, a > b) GBD(0x65, a <= b) GBD(0x66, a >= b)
        GU(0x67, clz32((u32)a)) GU(0x68, ctz32((u32)a)) GU(0x69, pop32((u32)a))
        case 0x6D: L_0x6D: { u32 b = (u32)s[-1], a = (u32)s[-2]; DIVCHK(!b) if ((int)a == (int)0x80000000 && (int)b == -1) { trap("integer overflow"); return; } s--; s[-1] = (u32)((int)a / (int)b); continue; }
        case 0x6E: L_0x6E: { u32 b = (u32)s[-1], a = (u32)s[-2]; DIVCHK(!b) s--; s[-1] = a / b; continue; }
        case 0x6F: L_0x6F: { u32 b = (u32)s[-1], a = (u32)s[-2]; DIVCHK(!b) s--; s[-1] = ((int)b == -1) ? 0 : (u32)((int)a % (int)b); continue; }
        case 0x70: L_0x70: { u32 b = (u32)s[-1], a = (u32)s[-2]; DIVCHK(!b) s--; s[-1] = a % b; continue; }
        GB32(0x77, (a << (b & 31)) | (a >> ((32 - (b & 31)) & 31))) GB32(0x78, (a >> (b & 31)) | (a << ((32 - (b & 31)) & 31)))
        GU(0x79, clz64(a)) GU(0x7A, ctz64(a)) GU(0x7B, pop64(a))
        GB64(0x7C, a + b) GB64(0x7D, a - b) GB64(0x7E, a * b)
        case 0x7F: L_0x7F: { u64 b = s[-1], a = s[-2]; DIVCHK(!b) if (a == 0x8000000000000000ull && (i64)b == -1) { trap("integer overflow"); return; } s--; s[-1] = (u64)((i64)a / (i64)b); continue; }
        case 0x80: L_0x80: { u64 b = s[-1], a = s[-2]; DIVCHK(!b) s--; s[-1] = a / b; continue; }
        case 0x81: L_0x81: { u64 b = s[-1], a = s[-2]; DIVCHK(!b) s--; s[-1] = ((i64)b == -1) ? 0 : (u64)((i64)a % (i64)b); continue; }
        case 0x82: L_0x82: { u64 b = s[-1], a = s[-2]; DIVCHK(!b) s--; s[-1] = a % b; continue; }
        GB64(0x83, a & b) GB64(0x84, a | b) GB64(0x85, a ^ b) GB64(0x86, a << (b & 63)) GB64(0x87, (u64)((i64)a >> (b & 63)))
        GB64(0x88, a >> (b & 63)) GB64(0x89, (a << (b & 63)) | (a >> ((64 - (b & 63)) & 63))) GB64(0x8A, (a >> (b & 63)) | (a << ((64 - (b & 63)) & 63)))
        GU(0x8B, (u32)a & 0x7FFFFFFFu) GU(0x8C, (u32)a ^ 0x80000000u) GU(0x8D, pf(ceil_f(gf(a)))) GU(0x8E, pf(floor_f(gf(a))))
        GU(0x8F, pf(trunc_f(gf(a)))) GU(0x90, pf(nearest_f(gf(a)))) GU(0x91, pf(__builtin_sqrtf(gf(a))))
        GBF(0x96, pf(min_f(a, b))) GBF(0x97, pf(max_f(a, b)))
        GB32(0x98, (a & 0x7FFFFFFFu) | (b & 0x80000000u))
        GU(0x99, a & 0x7FFFFFFFFFFFFFFFull) GU(0x9A, a ^ 0x8000000000000000ull) GU(0x9B, pd(ceil_d(gd(a)))) GU(0x9C, pd(floor_d(gd(a))))
        GU(0x9D, pd(trunc_d(gd(a)))) GU(0x9E, pd(nearest_d(gd(a)))) GU(0x9F, pd(__builtin_sqrt(gd(a))))
        GBD(0xA0, pd(a + b)) GBD(0xA1, pd(a - b)) GBD(0xA2, pd(a * b)) GBD(0xA3, pd(a / b)) GBD(0xA4, pd(min_d(a, b))) GBD(0xA5, pd(max_d(a, b)))
        GB64(0xA6, (a & 0x7FFFFFFFFFFFFFFFull) | (b & 0x8000000000000000ull))
        case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAE: case 0xAF: case 0xB0: case 0xB1: L_TRUNC: {   /* в целое: NaN/вне диапазона - ловушка */
            u32 op = w & 0x3FFF; u64 v = s[-1];
            if (op == 0xA8) { float f = gf(v); if (f >= -2147483648.0f && f < 2147483648.0f) { u64 r = (u32)(int)f; s[-1] = r; FIN(r) } }   /* быстрый путь: в диапазоне */
            double x = (op == 0xA8 || op == 0xA9 || op == 0xAE || op == 0xAF) ? (double)gf(v) : gd(v);
            int bits = op <= 0xAB ? 32 : 64, sgn = (op - (op <= 0xAB ? 0xA8 : 0xAE)) % 2 == 0;
            u64 r;
            if (!to_int(x, bits, sgn, &r)) { trap(x != x ? "invalid conversion to integer (NaN)" : "integer overflow in float-to-int conversion"); return; }
            s[-1] = r; NEXT;
        }
        GU(0xB2, pf((float)(int)(u32)a)) GU(0xB3, pf(u64_to_f((u32)a))) GU(0xB4, pf((float)(i64)a)) GU(0xB5, pf(u64_to_f(a)))
        GU(0xB6, pf((float)gd(a))) GU(0xB7, pd((double)(int)(u32)a)) GU(0xB8, pd((double)(u32)a)) GU(0xB9, pd((double)(i64)a))
        GU(0xBA, pd(u64_to_d(a))) GU(0xBB, pd((double)gf(a)))
        GU(0xA7, (u32)a) GU(0xAC, (u64)(i64)(int)(u32)a) GU(0xAD, (u64)(u32)a)
        GU(0xBC, (u32)a) GU(0xBD, a) GU(0xBE, (u32)a) GU(0xBF, a)           /* перенос битов: f32 <-> i32, f64 <-> i64 */
        GU(0xC0, (u32)(int)(signed char)a) GU(0xC1, (u32)(int)(short)a) GU(0xC2, (u64)(i64)(signed char)a)
        GU(0xC3, (u64)(i64)(short)a) GU(0xC4, (u64)(i64)(int)a)
        case OP_FC + 0: case OP_FC + 1: case OP_FC + 2: case OP_FC + 3: case OP_FC + 4: case OP_FC + 5: case OP_FC + 6: case OP_FC + 7: {   /* насыщающие преобразования */
            u32 sub = op - OP_FC;
            u64 v = s[-1]; double x = (sub == 0 || sub == 1 || sub == 4 || sub == 5) ? (double)gf(v) : gd(v);
            u64 r; to_int(x, sub <= 3 ? 32 : 64, sub % 2 == 0, &r);
            s[-1] = r; continue;
        }
        case OP_FC + 10: {                                        /* memory.copy */
            u32 n = (u32)s[-1], src = (u32)s[-2], d = (u32)s[-3]; s -= 3;
            if ((u64)src + n > ml || (u64)d + n > ml) { trap("memory.copy out of bounds"); return; }
            if (d < src) for (u32 i = 0; i < n; i++) m[d + i] = m[src + i]; else for (u32 i = n; i > 0; i--) m[d + i - 1] = m[src + i - 1];
            continue;
        }
        case OP_FC + 11: {                                        /* memory.fill */
            u32 n = (u32)s[-1], v = (u32)s[-2], d = (u32)s[-3]; s -= 3;
            if ((u64)d + n > ml) { trap("memory.fill out of bounds"); return; }
            for (u32 i = 0; i < n; i++) m[d + i] = (u8)v;
            continue;
        }
        default: trap("unknown instruction"); return;
        }
        }
    }
oob:
    trap("memory access out of bounds");
}

#if defined(__x86_64__)
#include "wasm_jit_x64.h"
#endif

int umain(void) {
    char path[128];
    if (k_grant_name(path, sizeof path) <= 0) { say("wasm: usage: wasm /path/module.wasm (or double click in Files)\n"); return 1; }
    i64 sz = k_grant_size();
    if (sz <= 0 || sz > (8 << 20)) { say("wasm: module not readable or larger than 8 MB\n"); return 1; }
    u8* buf = (u8*)alloc((u64)sz);
    vs = (u64*)alloc(8ull * VSTK);
    if (!buf || !vs) { say("wasm: no memory\n"); return 1; }
    u64 got = 0;
    while (got < (u64)sz) { i64 r = k_grant_read(buf + got, (u64)sz - got > 65536 ? 65536 : (u64)sz - got, got); if (r <= 0) break; got += (u64)r; }
    if (got != (u64)sz) {
        say("wasm: incomplete module read\n");
        return 1;
    }
    mod = buf; modlen = (u32)got;
    if (load() == 0) {
        say("wasm: "); say(path); say(" - "); say_u(nfuncs - nimports); say(" functions, "); say_u(nimports); say(" imports, memory ");
        say_u(mem_pages * 64); say(" KB\n");
        const ftype_t* t = &types[funcs[entry].type];
        for (u32 i = 0; i < t->np; i++) vs[sp++] = 0;              /* main(argc=0, argv=0) */
        u64 t0 = k_clock_ns();
#if defined(__x86_64__)
        int jit_ok = jit_build() == 0;
        if (jit_ok) { say("wasm: x86-64 native code, "); say_u(jit_code_size); say(" bytes (JIT)\n"); jit_exec(entry); }
        else
#endif
        { say("wasm: interpreter mode\n"); execute(entry); }
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
