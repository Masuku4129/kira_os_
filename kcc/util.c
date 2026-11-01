/* kcc: арена, диагностика, строковый буфер, точка входа библиотеки. */
#include "kcc.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

KCtx K;

void *ka_new(size_t n) {
    if (n > ((size_t)1 << 40)) n = (size_t)1 << 40;            /* заведомо больше любой арены: уйдёт в отказ ниже */
    n = (n + 7) & ~(size_t)7;
    if (n > K.acap - K.aused) {
        size_t nc = K.cfg && K.cfg->grow ? K.cfg->grow(K.cfg->ud, K.acap, K.aused + n) : 0;
        if (nc < K.aused + n) {
            snprintf(K.err, sizeof K.err, "error: arena exhausted (limit %zu bytes)", K.acap);
            longjmp(K.jb, 1);
        }
        K.acap = nc;
    }
    void *p = K.abase + K.aused;
    K.aused += n;
    memset(p, 0, n);
    return p;
}

char *ka_strdup(const char *s, size_t n) {
    char *p = ka_new(n + 1);
    memcpy(p, s, n);
    return p;
}

static void vfail(const char *file, int line, const char *fmt, va_list ap) {
    int n = snprintf(K.err, sizeof K.err, "%s:%d: error: ", file ? file : "<input>", line);
    if (n < 0 || (size_t)n >= sizeof K.err) n = 0;
    vsnprintf(K.err + n, sizeof K.err - (size_t)n, fmt, ap);
    longjmp(K.jb, 1);
}

void kerr(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfail(file, line, fmt, ap);
    va_end(ap);
    __builtin_unreachable();
}

void kerr_tok(const Token *t, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfail(t ? t->file : NULL, t ? t->line : 0, fmt, ap);
    va_end(ap);
    __builtin_unreachable();
}

void sb_add(SB *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n + 1) nc *= 2;
        char *p = ka_new(nc);
        if (b->len) memcpy(p, b->p, b->len);
        b->p = p; b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

void sb_printf(SB *b, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof tmp) n = (int)sizeof tmp - 1;
    sb_add(b, tmp, (size_t)n);
}

char *kfmt(const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof tmp) n = (int)sizeof tmp - 1;
    return ka_strdup(tmp, (size_t)n);
}

void kwarn(const char *file, int line, const char *fmt, ...) {
    char tmp[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    sb_printf(&K.warn, "%s:%d: warning: %s\n", file ? file : "<input>", line, tmp);
}

int kcc_compile(const KCfg *cfg, void *mem, size_t memsz, const char *path, int emit_ir, KResult *res) {
    memset(&K, 0, sizeof K);
    memset(res, 0, sizeof *res);
    K.abase = mem; K.acap = memsz; K.cfg = cfg;
    K.tokbudget = 4000000;
    if (setjmp(K.jb)) {
        res->err = K.err;
        res->warn = K.warn.p;
        return 1;
    }
    K.srcpath = path;
    Token *t = preprocess(path);
    Obj *prog = parse_program(t);
    if (emit_ir == 1) {
        SB out = {0};
        ir_dump(prog, &out);
        res->ir = out.p ? out.p : "";
    } else if (emit_ir == 2) {
        unsigned char *elf; size_t n;
        if (cfg->target && !strcmp(cfg->target, "aarch64")) a64_build_elf(prog, &elf, &n);
        else if (!cfg->target || !strcmp(cfg->target, "x86_64")) x86_build_elf(prog, &elf, &n);
        else kerr(path, 1, "unknown target '%s' (x86_64 or aarch64)", cfg->target);
        res->elf = elf; res->elflen = n;
    }
    res->warn = K.warn.p;
    return 0;
}
