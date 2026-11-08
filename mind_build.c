/* Задание на сборку: шаблон -> исходник -> квоты -> kcc -> проверка ELF -> песочница; разрешение человека или подписи.
   Описание, границы и порядок - в mind_build.h. Модуль для Ring 3 / хоста (libc-lite: snprintf, string.h), в ядро не входит. */
#include "mind_build.h"
#include <stdio.h>
#include <string.h>
#include "elf.h"
#include "immune.h"
#include "kcc/kcc.h"
#include "userland/drvget_core.h"

#define SRC_PATH "mbjob.c"

const char* mbj_strerror(int st) {
    switch (st) {
    case MBJ_OK:            return "ok";
    case MBJ_E_ARG:         return "неизвестный шаблон или параметр вне диапазона";
    case MBJ_E_QUOTA:       return "квота выше жёсткого предела";
    case MBJ_E_SRC_SIZE:    return "исходник больше квоты";
    case MBJ_E_SRC_FORBIDDEN: return "в исходнике запрещённая конструкция";
    case MBJ_E_TOKENS:      return "лексем больше квоты";
    case MBJ_E_COMPILE:     return "kcc отверг исходник";
    case MBJ_E_ARENA:       return "арена компилятора исчерпана";
    case MBJ_E_ELF_SIZE:    return "образ больше квоты";
    case MBJ_E_ELF_INVALID: return "образ не прошёл проверку ELF";
    case MBJ_E_TIME:        return "сборка дольше квоты времени";
    case MBJ_E_IO:          return "ошибка записи или чтения песочницы";
    case MBJ_E_NOREC:       return "нет записи о сборке или образа";
    case MBJ_E_NOGRANT:     return "разрешения нет";
    case MBJ_E_TAMPER:      return "образ изменён после сборки или разрешения";
    case MBJ_E_CONFIRM:     return "введённый хеш не совпал с SHA-256 образа";
    case MBJ_E_SIG:         return "подпись отвергнута";
    case MBJ_E_RATE:        return "слишком рано или слишком много ожидающих решения";
    default:                return "неизвестная ошибка";
    }
}

/* ---------------- шаблоны ---------------- */

int mbj_render(const mbj_job_t* job, char* out, size_t cap) {
    if (!job || !out || !cap) return MBJ_E_ARG;
    int n;
    if (job->tmpl == MBJ_T_PROBE) {
        uint32_t iters = job->p[0], seed = job->p[1];
        if (iters < 1 || iters > 4194304u) return MBJ_E_ARG;
        n = snprintf(out, cap,
            "int main(void) {\n"
            "    unsigned x = %uu;\n"
            "    unsigned acc = 0;\n"
            "    for (unsigned i = 0; i < %uu; i++) {\n"
            "        x = x * 1664525u + 1013904223u;\n"
            "        acc ^= x >> 7;\n"
            "        acc += i;\n"
            "    }\n"
            "    return (int)(acc & 0x7fffffffu);\n"
            "}\n", (unsigned)seed, (unsigned)iters);
    } else if (job->tmpl == MBJ_T_TABLE) {
        uint32_t cnt = job->p[0], mul = job->p[1], mod = job->p[2];
        if (cnt < 1 || cnt > 256 || mod < 1 || mod > 65536u) return MBJ_E_ARG;
        size_t pos = 0;
        n = snprintf(out, cap, "static const unsigned short T[%u] = {", (unsigned)cnt);
        if (n < 0 || (size_t)n >= cap) return MBJ_E_SRC_SIZE;
        pos = (size_t)n;
        for (uint32_t i = 0; i < cnt; i++) {
            uint32_t v = (uint32_t)(((uint64_t)i * mul + 1) % mod);
            n = snprintf(out + pos, cap - pos, "%s%u", i ? "," : "", (unsigned)v);
            if (n < 0 || (size_t)n >= cap - pos) return MBJ_E_SRC_SIZE;
            pos += (size_t)n;
        }
        n = snprintf(out + pos, cap - pos,
            "};\n"
            "int main(void) {\n"
            "    unsigned s = 0;\n"
            "    for (unsigned i = 0; i < %uu; i++) s += T[i] * (i + 1u);\n"
            "    return (int)(s & 0x7fffffffu);\n"
            "}\n", (unsigned)cnt);
        if (n < 0 || (size_t)n >= cap - pos) return MBJ_E_SRC_SIZE;
        return (int)(pos + (size_t)n);
    } else {
        return MBJ_E_ARG;
    }
    if (n < 0 || (size_t)n >= cap) return MBJ_E_SRC_SIZE;
    return n;
}

/* ---------------- ворота исходника ---------------- */

static int is_idc(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

static int forbid(char* why, size_t cap, const char* m) {
    if (why && cap) snprintf(why, cap, "%s", m);
    return MBJ_E_SRC_FORBIDDEN;
}

static int resolve_quota(const mbj_quota_t* in, mbj_quota_t* q) {
    q->arena_bytes = in && in->arena_bytes ? in->arena_bytes : MBJ_DEF_ARENA;
    q->max_src     = in && in->max_src     ? in->max_src     : MBJ_DEF_SRC;
    q->max_tokens  = in && in->max_tokens  ? in->max_tokens  : MBJ_DEF_TOKENS;
    q->max_elf     = in && in->max_elf     ? in->max_elf     : MBJ_DEF_ELF;
    q->max_ms      = in && in->max_ms      ? in->max_ms      : MBJ_DEF_MS;
    if (q->arena_bytes > MBJ_HARD_ARENA || q->max_src > MBJ_HARD_SRC || q->max_tokens > MBJ_HARD_TOKENS ||
        q->max_elf > MBJ_HARD_ELF || q->max_ms > MBJ_HARD_MS) return MBJ_E_QUOTA;
    return MBJ_OK;
}

int mbj_source_gate(const char* src, size_t len, const mbj_quota_t* qin, uint32_t* tokens, char* why, size_t why_cap) {
    mbj_quota_t q;
    int rq = resolve_quota(qin, &q);
    if (rq) return rq;
    if (tokens) *tokens = 0;
    if (!src || len == 0) return forbid(why, why_cap, "пустой исходник");
    if (len > q.max_src) return MBJ_E_SRC_SIZE;
    uint32_t tk = 0;
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)src[i];
        if (c == 0 || c >= 0x80 || (c < 0x20 && c != '\t' && c != '\n' && c != '\r')) return forbid(why, why_cap, "управляющий байт или не ASCII");
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
        if (c == '#') return forbid(why, why_cap, "директивы препроцессора запрещены");
        if (c == '\\') return forbid(why, why_cap, "обратная косая вне литерала запрещена");
        if (c == '?' && i + 1 < len && src[i + 1] == '?') return forbid(why, why_cap, "триграфы запрещены");
        if (c == '%' && i + 1 < len && src[i + 1] == ':') return forbid(why, why_cap, "диграфы запрещены");
        if (c == '/' && i + 1 < len && src[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < len && !(src[j] == '*' && src[j + 1] == '/')) j++;
            if (j + 1 >= len) return forbid(why, why_cap, "незакрытый комментарий");
            i = j + 2;
            continue;
        }
        if (c == '/' && i + 1 < len && src[i + 1] == '/') {
            while (i < len && src[i] != '\n') i++;
            continue;
        }
        if (is_idc((char)c) && !(c >= '0' && c <= '9')) {
            size_t j = i;
            while (j < len && is_idc(src[j])) j++;
            size_t wl = j - i;
            if ((wl == 3 && !memcmp(src + i, "asm", 3)) || (wl == 5 && !memcmp(src + i, "__asm", 5)) ||
                (wl == 7 && !memcmp(src + i, "__asm__", 7)) || (wl == 7 && !memcmp(src + i, "_Pragma", 7)))
                return forbid(why, why_cap, "asm и _Pragma запрещены");
            i = j;
        } else if (c >= '0' && c <= '9') {
            size_t j = i;
            while (j < len && (is_idc(src[j]) || src[j] == '.')) j++;
            i = j;
        } else if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < len && src[j] != (char)c) {
                if (src[j] == '\n') return forbid(why, why_cap, "незакрытый литерал");
                if (src[j] == '\\') j++;
                j++;
            }
            if (j >= len) return forbid(why, why_cap, "незакрытый литерал");
            i = j + 1;
        } else {
            i++;                                              /* знак пунктуации: каждый символ считается лексемой (с запасом) */
        }
        if (++tk > q.max_tokens) {
            if (tokens) *tokens = tk;
            return MBJ_E_TOKENS;
        }
    }
    if (tokens) *tokens = tk;
    return MBJ_OK;
}

/* ---------------- сборка ---------------- */

typedef struct { const char* src; size_t len; } srcctx_t;
static const char* rd_job(void* ud, const char* path, size_t* len) {
    const srcctx_t* c = (const srcctx_t*)ud;
    if (strcmp(path, SRC_PATH)) return 0;                      /* никаких других файлов: ни #include, ни чтения песочницы */
    *len = c->len;
    return c->src;
}

static void put_err(mbj_result_t* r, const char* m) { snprintf(r->err, sizeof r->err, "%s", m); }

static int fail(mbj_result_t* r, int st, const char* detail) {
    r->status = st;
    if (detail) put_err(r, detail); else put_err(r, mbj_strerror(st));
    return st;
}

static int name_valid(const char* n) {
    size_t k = n ? strlen(n) : 0;
    if (k < 4 || k > 32 || memcmp(n, "mb-", 3)) return 0;
    for (size_t i = 0; i < k; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0;
    }
    return 1;
}

static void side_name(char* out, size_t cap, const char* name, const char* ext) { snprintf(out, cap, "%s.%s", name, ext); }

int mbj_build_source(const mbj_env_t* env, const mbj_quota_t* qin, uint32_t tmpl, const char* src, size_t len, mbj_result_t* res) {
    memset(res, 0, sizeof *res);
    if (!env || !env->now_ms || !env->put || !env->arena) return fail(res, MBJ_E_ARG, "окружение задано неполно");
    mbj_quota_t q;
    int rc = resolve_quota(qin, &q);
    if (rc) return fail(res, rc, 0);
    char why[120] = { 0 };
    rc = mbj_source_gate(src, len, &q, &res->tokens, why, sizeof why);
    res->src_len = (uint32_t)len;
    if (rc) return fail(res, rc, why[0] ? why : 0);

    uint64_t t0 = env->now_ms(env->ud);
    size_t arena = q.arena_bytes < env->arena_cap ? q.arena_bytes : env->arena_cap;
    srcctx_t sc = { src, len };
    KCfg cfg = { rd_job, &sc, 0, 0, 0, 0, env->target ? env->target : "x86_64", 0 };   /* grow = 0: арена не растёт */
    KResult kr;
    int krc = kcc_compile(&cfg, env->arena, arena, SRC_PATH, 2, &kr);
    res->arena_used = (uint32_t)K.aused;
    if (krc) {
        if (strstr(kr.err, "arena exhausted")) return fail(res, MBJ_E_ARENA, kr.err);
        return fail(res, MBJ_E_COMPILE, kr.err);
    }
    if (!kr.elf || kr.elflen == 0) return fail(res, MBJ_E_COMPILE, "kcc не вернул образ");
    if (kr.elflen > q.max_elf) return fail(res, MBJ_E_ELF_SIZE, 0);
    elf_info_t info;
    int er = elf_parse(kr.elf, kr.elflen, env->win_lo, env->win_hi, env->max_pages, &info);
    if (er != ELF_OK) return fail(res, MBJ_E_ELF_INVALID, elf_strerror(er));

    sha256(kr.elf, kr.elflen, res->elf_sha);
    dg_hex(res->elf_sha, 32, res->elf_hex);
    uint8_t ssha[32];
    sha256(src, len, ssha);
    char sh[9];
    dg_hex(ssha, 4, sh);
    snprintf(res->name, sizeof res->name, "mb-t%u-%s", (unsigned)tmpl, sh);
    res->elf_len = kr.elflen;
    uint64_t t1 = env->now_ms(env->ud);
    res->ms = (uint32_t)(t1 - t0);
    if (t1 - t0 > q.max_ms) return fail(res, MBJ_E_TIME, 0);     /* образ отбрасывается, в песочницу не пишется */

    mbj_rec_t rec;
    memset(&rec, 0, sizeof rec);
    rec.magic = MBJ_REC_MAGIC; rec.version = 1; rec.tmpl = tmpl; rec.state = MBJ_STATE_PENDING;
    rec.elf_len = (uint32_t)kr.elflen; rec.src_len = (uint32_t)len;
    memcpy(rec.elf_sha, res->elf_sha, 32);
    memcpy(rec.src_sha, ssha, 32);
    char rn[48];
    side_name(rn, sizeof rn, res->name, "mbj");
    if (env->put(env->ud, res->name, kr.elf, kr.elflen) != 0) return fail(res, MBJ_E_IO, 0);
    if (env->put(env->ud, rn, &rec, sizeof rec) != 0) return fail(res, MBJ_E_IO, 0);
    res->status = MBJ_OK;
    snprintf(res->err, sizeof res->err, "собрано, ожидает решения человека или подписи; запуск не выполнялся");
    return MBJ_OK;
}

int mbj_build(const mbj_env_t* env, const mbj_quota_t* q, const mbj_job_t* job, mbj_result_t* res) {
    char src[MBJ_HARD_SRC];
    memset(res, 0, sizeof *res);
    if (!job) return fail(res, MBJ_E_ARG, 0);
    int n = mbj_render(job, src, sizeof src);
    if (n < 0) return fail(res, n, 0);
    return mbj_build_source(env, q, job->tmpl, src, (size_t)n, res);
}

/* ---------------- разрешения ---------------- */

static int load_rec(const mbj_env_t* env, const char* name, void* buf, size_t cap, mbj_rec_t* rec, size_t* elen) {
    if (!env || !env->get || !env->put || !name_valid(name) || !buf) return MBJ_E_ARG;
    char rn[48];
    side_name(rn, sizeof rn, name, "mbj");
    int64_t n = env->get(env->ud, rn, rec, sizeof *rec);
    if (n != (int64_t)sizeof *rec) return MBJ_E_NOREC;
    if (rec->magic != MBJ_REC_MAGIC || rec->version != 1 || rec->state != MBJ_STATE_PENDING || rec->elf_len == 0) return MBJ_E_NOREC;
    if (rec->elf_len > cap) return MBJ_E_ELF_SIZE;
    int64_t e = env->get(env->ud, name, buf, cap);
    if (e < 0) return MBJ_E_NOREC;
    if ((uint64_t)e != rec->elf_len) return MBJ_E_TAMPER;
    uint8_t d[32];
    sha256(buf, (size_t)e, d);
    if (memcmp(d, rec->elf_sha, 32)) return MBJ_E_TAMPER;
    elf_info_t info;
    int er = elf_parse(buf, (size_t)e, env->win_lo, env->win_hi, env->max_pages, &info);
    if (er != ELF_OK) return MBJ_E_ELF_INVALID;
    *elen = (size_t)e;
    return MBJ_OK;
}

static int write_grant(const mbj_env_t* env, const char* name, uint32_t kind, const mbj_rec_t* rec) {
    mbj_grant_t g;
    memset(&g, 0, sizeof g);
    g.magic = MBJ_GRANT_MAGIC; g.kind = kind; g.elf_len = rec->elf_len;
    memcpy(g.elf_sha, rec->elf_sha, 32);
    char gn[48];
    side_name(gn, sizeof gn, name, "ok");
    return env->put(env->ud, gn, &g, sizeof g) == 0 ? MBJ_OK : MBJ_E_IO;
}

int mbj_authorize_human(const mbj_env_t* env, const char* name, const char* typed_hex, void* buf, size_t cap) {
    mbj_rec_t rec;
    size_t n;
    int rc = load_rec(env, name, buf, cap, &rec, &n);
    if (rc) return rc;
    if (!typed_hex || !dg_hex_prefix(typed_hex, rec.elf_sha)) return MBJ_E_CONFIRM;
    return write_grant(env, name, MBJ_GRANT_HUMAN, &rec);
}

int mbj_authorize_signed(const mbj_env_t* env, const char* name, const void* sig, size_t siglen,
                         const kdrv_sig_key_t* keys, uint32_t nkeys, void* buf, size_t cap) {
    mbj_rec_t rec;
    size_t n;
    int rc = load_rec(env, name, buf, cap, &rec, &n);
    if (rc) return rc;
    if (!env->now_utc || !sig || !keys || !nkeys) return MBJ_E_ARG;
    if (kdrv_sig_verify(buf, n, sig, siglen, keys, nkeys, env->now_utc(env->ud)) != KDRV_SIG_OK) return MBJ_E_SIG;
    return write_grant(env, name, MBJ_GRANT_SIGNED, &rec);
}

int mbj_check_grant(const mbj_env_t* env, const char* name, void* buf, size_t cap, size_t* len) {
    mbj_rec_t rec;
    size_t n;
    int rc = load_rec(env, name, buf, cap, &rec, &n);
    if (rc) return rc;
    mbj_grant_t g;
    char gn[48];
    side_name(gn, sizeof gn, name, "ok");
    int64_t gl = env->get(env->ud, gn, &g, sizeof g);
    if (gl != (int64_t)sizeof g) return MBJ_E_NOGRANT;
    if (g.magic != MBJ_GRANT_MAGIC || (g.kind != MBJ_GRANT_HUMAN && g.kind != MBJ_GRANT_SIGNED)) return MBJ_E_NOGRANT;
    if (g.elf_len != rec.elf_len || memcmp(g.elf_sha, rec.elf_sha, 32)) return MBJ_E_TAMPER;
    if (len) *len = n;
    return MBJ_OK;
}

/* ---------------- темп и правило Разума ---------------- */

int mbj_rate_allow(const mbj_rate_t* r, uint32_t now_s) {
    if (r->pending >= r->max_pending) return 0;
    if (r->any && now_s - r->last_s < r->min_gap_s) return 0;
    return 1;
}
void mbj_rate_note_built(mbj_rate_t* r, uint32_t now_s) { r->last_s = now_s; r->any = 1; r->pending++; }
void mbj_rate_note_decided(mbj_rate_t* r) { if (r->pending) r->pending--; }

int mind_build_propose(const mind_t* s, const mind_sample_t* in, uint32_t now_s, const mbj_rate_t* r, mbj_job_t* job) {
    if (s->m.mode != MIND_ACT || s->health < 80 || s->late_s || s->calm_s < 30 || s->tact_on || s->mem_level) return 0;
    if (!in->tsc_khz || !mbj_rate_allow(r, now_s)) return 0;
    uint64_t it = (uint64_t)in->tsc_khz * 5 / 6;               /* ~10 мс цикла при ~12 тактах на итерацию */
    if (it < 1000) it = 1000;
    if (it > 4194304u) it = 4194304u;
    job->tmpl = MBJ_T_PROBE; job->p[0] = (uint32_t)it; job->p[1] = in->t; job->p[2] = 0;
    return 1;
}
