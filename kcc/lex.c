/* kcc: лексер. Склейка строк (\ + перевод строки) снимается до разбора; номера строк сохраняются. */
#include "kcc.h"
#include <string.h>

int tok_is(const Token *t, const char *s) {
    size_t n = strlen(s);
    return t->len == (int)n && memcmp(t->s, s, n) == 0 && (t->k == TK_PUNCT || t->k == TK_KW || t->k == TK_ID);
}

static int is_idc(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$'; }
static int is_dig(int c) { return c >= '0' && c <= '9'; }

static const char *const punct3[] = { "<<=", ">>=", "...", 0 };
static const char *const punct2[] = { "->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
                                      "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##", 0 };

Token *lex_buf(const char *file, const char *src, size_t len) {
    char *b = ka_new(len + 4);
    size_t n = 0; int pend = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\\' && i + 1 < len && src[i + 1] == '\n') { pend++; i++; continue; }
        if (src[i] == '\\' && i + 2 < len && src[i + 1] == '\r' && src[i + 2] == '\n') { pend++; i += 2; continue; }
        b[n++] = src[i];
        if (src[i] == '\n') while (pend) { b[n++] = '\n'; pend--; }
    }
    if (n == 0 || b[n - 1] != '\n') b[n++] = '\n';
    b[n] = 0;
    /* лишние перевода строк при склейке могут превысить len+4 - выделяем с запасом */
    Token head = {0}, *cur = &head;
    const char *p = b, *end = b + n;
    int line = 1, bol = 1, sp = 0;
    Token *prev = NULL, *pprev = NULL;
    #define MK(kind, st, ln) do { Token *t_ = ka_new(sizeof(Token)); t_->k = (kind); t_->s = (st); t_->len = (int)(ln); \
        t_->line = line; t_->file = file; t_->bol = (unsigned)bol; t_->sp = (unsigned)sp; cur->next = t_; cur = t_; pprev = prev; prev = t_; bol = 0; sp = 0; } while (0)
    while (p < end) {
        char c = *p;
        if (c == '\n') { line++; bol = 1; sp = 0; p++; continue; }
        if (c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r') { sp = 1; p++; continue; }
        if (c == '/' && p[1] == '/') { while (p < end && *p != '\n') p++; sp = 1; continue; }
        if (c == '/' && p[1] == '*') {
            int l0 = line; const char *q = p + 2;
            while (q + 1 < end && !(q[0] == '*' && q[1] == '/')) { if (*q == '\n') line++; q++; }
            if (q + 1 >= end) kerr(file, l0, "unterminated /* comment");
            p = q + 2; sp = 1; continue;
        }
        if (is_idc(c) && !is_dig(c)) {
            const char *q = p;
            while (q < end && is_idc(*q)) q++;
            int l = (int)(q - p);
            if (*q == '"' || *q == '\'') {
                int wide = !((l == 2 && p[0] == 'u' && p[1] == '8' && *q == '"')) && ((l == 1 && (*p == 'L' || *p == 'u' || *p == 'U')) || (l == 2 && p[0] == 'u' && p[1] == '8'));
                if (wide || (l == 2 && p[0] == 'u' && p[1] == '8')) {
                    if (wide) {
                        const char *r = q + 1;
                        while (r < end && *r != *q && *r != '\n') { if (*r == '\\' && r[1] != '\n') r++; r++; }
                        if (r < end && *r == *q) r++;
                        MK(*q == '"' ? TK_STR : TK_CHR, p, r - p);
                        cur->mark = 1;
                        p = r; continue;
                    }
                    p = q; /* u8"..." - обычная строка */
                    continue;
                }
            }
            MK(TK_ID, p, l);
            p = q;
            if (prev->len == 7 && memcmp(prev->s, "include", 7) == 0) {
                /* #include <...> - заголовок читается целиком */
                Token *hash = pprev;
                if (hash && hash->k == TK_PUNCT && hash->len == 1 && hash->s[0] == '#' && hash->bol) {
                    const char *r = p;
                    while (*r == ' ' || *r == '\t') r++;
                    if (*r == '<') {
                        const char *e2 = r + 1;
                        while (e2 < end && *e2 != '>' && *e2 != '\n') e2++;
                        if (*e2 == '>') {
                            sp = 1;
                            MK(TK_STR, r, e2 + 1 - r);
                            cur->hdr = 1;
                            p = e2 + 1;
                        }
                    }
                }
            }
            continue;
        }
        if (is_dig(c) || (c == '.' && is_dig(p[1]))) {
            const char *q = p;
            while (q < end) {
                if (is_idc(*q) || *q == '.') q++;
                else if ((*q == '+' || *q == '-') && q > p && (q[-1] == 'e' || q[-1] == 'E' || q[-1] == 'p' || q[-1] == 'P')) q++;
                else break;
            }
            MK(TK_NUM, p, q - p);
            p = q; continue;
        }
        if (c == '"' || c == '\'') {
            const char *q = p + 1;
            while (q < end && *q != c && *q != '\n') { if (*q == '\\' && q[1] != '\n') q++; q++; }
            if (q < end && *q == c) { MK(c == '"' ? TK_STR : TK_CHR, p, q + 1 - p); p = q + 1; continue; }
            MK(TK_PUNCT, p, 1);   /* незакрытый литерал: ошибка только если токен дойдёт до разбора */
            p++; continue;
        }
        int pl = 1;
        for (int i = 0; punct3[i]; i++) if (!strncmp(p, punct3[i], 3)) pl = 3;
        if (pl == 1) for (int i = 0; punct2[i]; i++) if (!strncmp(p, punct2[i], 2)) pl = 2;
        MK(TK_PUNCT, p, pl);
        p += pl;
    }
    Token *e = ka_new(sizeof(Token));
    e->k = TK_EOF; e->file = file; e->line = line; e->bol = 1; e->s = "";
    cur->next = e;
    return head.next;
    #undef MK
}

/* ---------- escape-последовательности, числа, ключевые слова ---------- */
int decode_escape(const Token *t, const char **pp) {
    const char *p = *pp;
    int c = (unsigned char)*p++;
    int v;
    switch (c) {
    case 'n': v = '\n'; break;
    case 't': v = '\t'; break;
    case 'r': v = '\r'; break;
    case 'a': v = 7; break;
    case 'b': v = 8; break;
    case 'f': v = 12; break;
    case 'v': v = 11; break;
    case 'e': v = 27; break;
    case '\\': case '\'': case '"': case '?': v = c; break;
    case 'x': {
        int n = 0; v = 0;
        for (;;) {
            int d = *p;
            if (d >= '0' && d <= '9') d -= '0';
            else if (d >= 'a' && d <= 'f') d = d - 'a' + 10;
            else if (d >= 'A' && d <= 'F') d = d - 'A' + 10;
            else break;
            v = (v * 16 + d) & 0xffff; p++; n++;
        }
        if (!n) kerr_tok(t, "\\x used with no following hex digits");
        v &= 0xff;
        break;
    }
    case 'u': case 'U': kerr_tok(t, "universal character names are not supported");
    default:
        if (c >= '0' && c <= '7') {
            v = c - '0';
            for (int i = 0; i < 2 && *p >= '0' && *p <= '7'; i++) v = v * 8 + (*p++ - '0');
            v &= 0xff;
        } else kerr_tok(t, "unknown escape sequence '\\%c'", c);
    }
    *pp = p;
    return v;
}

void parse_intlit(const Token *t, int64_t *val, Type **ty) {
    const char *p = t->s, *e = t->s + t->len;
    int base = 10;
    if (t->len > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p += 2; }
    else if (t->len > 2 && p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) { base = 2; p += 2; }
    else if (t->len > 1 && p[0] == '0') base = 8;
    for (const char *q = p; q < e; q++) {
        if (*q == '.' || (base != 16 && (*q == 'e' || *q == 'E')) || (base == 16 && (*q == 'p' || *q == 'P')))
            kerr_tok(t, "floating-point constants are not supported by kcc");
    }
    uint64_t v = 0; int nd = 0;
    for (; p < e; p++, nd++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (base == 16 && *p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (base == 16 && *p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else break;
        if (d >= base) kerr_tok(t, "invalid digit '%c' in %s constant", *p, base == 8 ? "octal" : "binary");
        if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) kerr_tok(t, "integer literal is too large");
        v = v * (uint64_t)base + (uint64_t)d;
    }
    if (nd == 0 && base != 8) kerr_tok(t, "invalid integer constant '%.*s'", t->len, t->s);
    int u = 0, l = 0;
    for (; p < e; p++) {
        if ((*p == 'u' || *p == 'U') && !u) u = 1;
        else if ((*p == 'l' || *p == 'L') && l == 0) { l = 1; if (p + 1 < e && p[1] == *p) { l = 2; p++; } }
        else kerr_tok(t, "invalid suffix on integer constant '%.*s'", t->len, t->s);
    }
    int dec = base == 10;
    Type *c[6]; int nc = 0;
    if (!u && l == 0) { c[nc++] = &ty_int; if (!dec) c[nc++] = &ty_uint; c[nc++] = &ty_long; if (!dec) c[nc++] = &ty_ulong; }
    else if (u && l == 0) { c[nc++] = &ty_uint; c[nc++] = &ty_ulong; }
    else if (!u && l == 1) { c[nc++] = &ty_long; if (!dec) c[nc++] = &ty_ulong; }
    else if (u && l == 1) { c[nc++] = &ty_ulong; }
    else if (!u) { c[nc++] = &ty_llong; if (!dec) c[nc++] = &ty_ullong; }
    else c[nc++] = &ty_ullong;
    Type *r = NULL;
    for (int i = 0; i < nc && !r; i++) {
        int bits = c[i]->size * 8;
        uint64_t mx = c[i]->is_unsigned ? (bits == 64 ? UINT64_MAX : (1ULL << bits) - 1) : (1ULL << (bits - 1)) - 1;
        if (v <= mx) r = c[i];
    }
    if (!r) r = (l == 2 || u) ? &ty_ullong : &ty_ulong;   /* десятичная константа, не влезшая в знаковый тип */
    *val = (int64_t)v; *ty = r;
}

static const char *const kwtab[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum", "extern",
    "float", "for", "goto", "if", "inline", "int", "long", "register", "restrict", "return", "short", "signed",
    "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while",
    "_Bool", "_Complex", "_Generic", "_Static_assert", "_Noreturn", "_Thread_local", "_Alignas", "_Atomic",
    "typeof", "asm", "__attribute__", "__extension__", "__alignof__", "__builtin_va_list", "__label__", 0 };
static const char *const kwsyn[][2] = {
    { "__const", "const" }, { "__const__", "const" }, { "__volatile", "volatile" }, { "__volatile__", "volatile" },
    { "__inline", "inline" }, { "__inline__", "inline" }, { "__signed", "signed" }, { "__signed__", "signed" },
    { "__restrict", "restrict" }, { "__restrict__", "restrict" }, { "__asm", "asm" }, { "__asm__", "asm" },
    { "__typeof", "typeof" }, { "__typeof__", "typeof" }, { "__attribute", "__attribute__" },
    { "_Alignof", "__alignof__" }, { "__alignof", "__alignof__" }, { 0, 0 } };

static const char *kw_lookup(const Token *t) {
    for (int i = 0; kwsyn[i][0]; i++)
        if ((int)strlen(kwsyn[i][0]) == t->len && !memcmp(kwsyn[i][0], t->s, (size_t)t->len)) return kwsyn[i][1];
    for (int i = 0; kwtab[i]; i++)
        if ((int)strlen(kwtab[i]) == t->len && !memcmp(kwtab[i], t->s, (size_t)t->len)) return kwtab[i];
    return NULL;
}

void conv_char(Token *t) {
    const char *p = t->s + 1;
    int64_t v = 0; int n = 0;
    while (*p != '\'') {
        int c;
        if (*p == '\\') { p++; c = decode_escape(t, &p); }
        else c = (unsigned char)*p++;
        v = (int32_t)((uint32_t)v << 8 | (uint32_t)(c & 0xff));
        n++;
    }
    if (n == 0) kerr_tok(t, "empty character constant");
    if (n == 1) v = (int8_t)v;                      /* char знаковый */
    else if (n > 4) kerr_tok(t, "character constant too long for its type");
    t->ival = (int32_t)v; t->ty = &ty_int;
}

void convert_tokens(Token *t) {
    for (; t->k != TK_EOF; t = t->next) {
        if (t->k == TK_ID) {
            const char *kw = kw_lookup(t);
            if (kw) { t->k = TK_KW; t->s = kw; t->len = (int)strlen(kw); }
        } else if (t->k == TK_NUM) {
            parse_intlit(t, &t->ival, &t->ty);
        } else if (t->k == TK_CHR) {
            if (t->mark) kerr_tok(t, "wide and Unicode character constants are not supported");
            conv_char(t);
        } else if (t->k == TK_STR) {
            if (t->mark) kerr_tok(t, "wide and Unicode string literals are not supported");
            if (t->hdr) kerr_tok(t, "unexpected header name");
            size_t cap = 0;
            for (Token *q = t; q->k == TK_STR; q = q->next) cap += (size_t)q->len;
            char *buf = ka_new(cap + 1);
            size_t n = 0;
            Token *last = t;
            for (Token *q = t; q->k == TK_STR; q = q->next) {
                if (q->mark) kerr_tok(q, "wide and Unicode string literals are not supported");
                const char *p = q->s + 1, *e = q->s + q->len - 1;
                while (p < e) {
                    if (*p == '\\') { p++; buf[n++] = (char)decode_escape(q, &p); }
                    else buf[n++] = *p++;
                }
                last = q;
            }
            t->str = buf; t->slen = (int)n;
            t->next = last->next;
        }
    }
}
