#include "json.h"

typedef struct { const char* s; size_t n, i; } P;

static void ws(P* p) { while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r')) p->i++; }
static int hex4(P* p) {
    if (p->i + 4 > p->n) return -1;
    int v = 0;
    for (int k = 0; k < 4; k++) {
        char c = p->s[p->i + k]; int d;
        if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10; else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else return -1;
        v = v * 16 + d;
    }
    p->i += 4; return v;
}
/* Строка: p->i на открывающей кавычке. out (необязательно) получает раскрытые байты; *len - их число. */
static int str(P* p, char* out, size_t cap, size_t* len) {
    size_t o = 0; int over = 0;
#define PUT(b) do { if (out) { if (o < cap) out[o] = (char)(b); else over = 1; } o++; } while (0)
    if (p->i >= p->n || p->s[p->i] != '"') return JSON_ERR_SYNTAX;
    p->i++;
    for (;;) {
        if (p->i >= p->n) return JSON_ERR_SYNTAX;
        unsigned char c = (unsigned char)p->s[p->i++];
        if (c == '"') break;
        if (c < 0x20) return JSON_ERR_SYNTAX;                       /* управляющие символы в строке запрещены */
        if (c != '\\') { PUT(c); continue; }
        if (p->i >= p->n) return JSON_ERR_SYNTAX;
        char e = p->s[p->i++];
        switch (e) {
        case '"': PUT('"'); break; case '\\': PUT('\\'); break; case '/': PUT('/'); break;
        case 'b': PUT('\b'); break; case 'f': PUT('\f'); break; case 'n': PUT('\n'); break; case 'r': PUT('\r'); break; case 't': PUT('\t'); break;
        case 'u': {
            int u = hex4(p); if (u < 0) return JSON_ERR_SYNTAX;
            if (u >= 0xD800 && u <= 0xDBFF) {                       /* старшая половина пары: обязательна младшая */
                if (p->i + 2 > p->n || p->s[p->i] != '\\' || p->s[p->i + 1] != 'u') return JSON_ERR_SYNTAX;
                p->i += 2; int lo = hex4(p);
                if (lo < 0xDC00 || lo > 0xDFFF) return JSON_ERR_SYNTAX;
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            } else if (u >= 0xDC00 && u <= 0xDFFF) return JSON_ERR_SYNTAX;   /* одинокая младшая половина */
            if (u < 0x80) PUT(u);
            else if (u < 0x800) { PUT(0xC0 | (u >> 6)); PUT(0x80 | (u & 63)); }
            else if (u < 0x10000) { PUT(0xE0 | (u >> 12)); PUT(0x80 | ((u >> 6) & 63)); PUT(0x80 | (u & 63)); }
            else { PUT(0xF0 | (u >> 18)); PUT(0x80 | ((u >> 12) & 63)); PUT(0x80 | ((u >> 6) & 63)); PUT(0x80 | (u & 63)); }
            break; }
        default: return JSON_ERR_SYNTAX;
        }
    }
    if (len) *len = o;
    return over ? JSON_ERR_BUF : JSON_OK;
#undef PUT
}
static int digits(P* p) { size_t s = p->i; while (p->i < p->n && p->s[p->i] >= '0' && p->s[p->i] <= '9') p->i++; return p->i > s; }
static int num(P* p) {
    if (p->i < p->n && p->s[p->i] == '-') p->i++;
    if (p->i >= p->n) return JSON_ERR_SYNTAX;
    if (p->s[p->i] == '0') p->i++;                                  /* без ведущих нулей */
    else if (!digits(p)) return JSON_ERR_SYNTAX;
    if (p->i < p->n && p->s[p->i] == '.') { p->i++; if (!digits(p)) return JSON_ERR_SYNTAX; }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        p->i++; if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
        if (!digits(p)) return JSON_ERR_SYNTAX;
    }
    return JSON_OK;
}
static int lit(P* p, const char* w) { size_t k = 0; while (w[k]) { if (p->i + k >= p->n || p->s[p->i + k] != w[k]) return 0; k++; } p->i += k; return 1; }

/* Значение с проверкой. path (необязательно) - остаток пути: если задан, в out кладётся найденное значение (разбор документа идёт до конца). */
static int val(P* p, int depth, const char* path, json_val* out);

static int path_step(const char** path, char* key, size_t cap, long* idx, int* is_idx) {
    const char* q = *path;
    if (*q == '.') q++;
    if (*q == '[') {
        q++; long v = 0; size_t d = 0;
        while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); if (++d > 9) return JSON_ERR_PATH; q++; }
        if (!d || *q != ']') return JSON_ERR_PATH;
        *path = q + 1; *idx = v; *is_idx = 1; return JSON_OK;
    }
    size_t k = 0;
    while (*q && *q != '.' && *q != '[') { if (k + 1 >= cap) return JSON_ERR_PATH; key[k++] = *q++; }
    if (!k) return JSON_ERR_PATH;
    key[k] = 0; *path = q; *is_idx = 0; return JSON_OK;
}

static int val(P* p, int depth, const char* path, json_val* out) {
    if (depth > JSON_MAX_DEPTH) return JSON_ERR_DEPTH;
    ws(p);
    if (p->i >= p->n) return JSON_ERR_SYNTAX;
    size_t start = p->i; int type, r = JSON_OK; char c = p->s[p->i];
    int want = path && *path;                                        /* нужно углубляться в этот контейнер */
    char key[48]; long want_idx = 0; int is_idx = 0, found = 0;
    json_val sub; sub.type = 0;
    if (c == '{' || c == '[') {
        type = c == '{' ? JSON_OBJ : JSON_ARR;
        const char* rest = path;
        if (want) { r = path_step(&rest, key, sizeof key, &want_idx, &is_idx); if (r) return r; }
        p->i++; ws(p);
        char close = c == '{' ? '}' : ']';
        long n = 0;
        if (p->i < p->n && p->s[p->i] == close) p->i++;
        else for (;;) {
            int match = 0;
            if (type == JSON_OBJ) {
                ws(p); char k2[48]; size_t kl = 0;
                r = str(p, k2, sizeof k2 - 1, &kl);
                if (r == JSON_ERR_BUF) { r = JSON_OK; kl = (size_t)-1; }   /* ключ длиннее буфера: совпасть с коротким путём не может */
                if (r) return r;
                if (want && !is_idx && kl != (size_t)-1 && !found) { k2[kl] = 0; size_t a = 0; while (key[a] && key[a] == k2[a]) a++; match = !key[a] && !k2[a] && kl == a; }
                ws(p); if (p->i >= p->n || p->s[p->i] != ':') return JSON_ERR_SYNTAX; p->i++;
            } else match = want && is_idx && n == want_idx;
            if (match) { r = val(p, depth + 1, rest, &sub); found = 1; }
            else r = val(p, depth + 1, 0, 0);
            if (r) return r;
            n++; ws(p);
            if (p->i >= p->n) return JSON_ERR_SYNTAX;
            if (p->s[p->i] == ',') { p->i++; continue; }
            if (p->s[p->i] == close) { p->i++; break; }
            return JSON_ERR_SYNTAX;
        }
        if (want) { if (!found) return JSON_ERR_NOTFOUND; if (out) *out = sub; return JSON_OK; }
    } else {
        if (want) { r = val(p, depth, 0, 0); return r ? r : JSON_ERR_NOTFOUND; }   /* путь просит войти в скаляр */
        if (c == '"') { type = JSON_STR; r = str(p, 0, 0, 0); }
        else if (c == 't') { type = JSON_BOOL; r = lit(p, "true") ? JSON_OK : JSON_ERR_SYNTAX; }
        else if (c == 'f') { type = JSON_BOOL; r = lit(p, "false") ? JSON_OK : JSON_ERR_SYNTAX; }
        else if (c == 'n') { type = JSON_NULL; r = lit(p, "null") ? JSON_OK : JSON_ERR_SYNTAX; }
        else if (c == '-' || (c >= '0' && c <= '9')) { type = JSON_NUM; r = num(p); }
        else return JSON_ERR_SYNTAX;
        if (r) return r;
    }
    if (out) { out->type = type; out->p = p->s + start; out->n = p->i - start; }
    return JSON_OK;
}

int json_validate(const char* s, size_t n) {
    P p = { s, n, 0 };
    int r = val(&p, 1, 0, 0); if (r) return r;
    ws(&p); return p.i == n ? JSON_OK : JSON_ERR_SYNTAX;
}
int json_get(const char* s, size_t n, const char* path, json_val* out) {
    int r = json_validate(s, n); if (r) return r;                    /* сначала весь документ: частично верный файл не читаем */
    P p = { s, n, 0 };
    return val(&p, 1, path, out);
}
long json_string(const json_val* v, char* out, size_t cap) {
    if (!v || v->type != JSON_STR || !cap) return JSON_ERR_SYNTAX;
    P p = { v->p, v->n, 0 }; size_t len = 0;
    int r = str(&p, out, cap - 1, &len); if (r) return r;
    out[len] = 0; return (long)len;
}
