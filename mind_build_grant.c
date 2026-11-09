/* Задание на сборку, сторона РЕШЕНИЯ: запись «ожидает», разрешение человека или подписи, проверка перед запуском, темп заданий,
   правило Разума. Работает в ядре (buildcmd.c) и на хосте. Компилятора и запуска здесь нет: запуск выполняет только консольная
   команда `build run` (buildcmd.c) по образу, который вернул mbj_check_grant. Без libc: только mem*, sha256 и elf_parse ядра. */
#include "mind_build_grant.h"
#include "elf.h"
#include "immune.h"

static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }

/* Без <string.h>: файл собирается и ядром (без libc), и хостом. Размеры здесь - десятки байт. */
static void cpy(void* d, const void* s, size_t n) { uint8_t* a = (uint8_t*)d; const uint8_t* b = (const uint8_t*)s; while (n--) *a++ = *b++; }
static void zero(void* d, size_t n) { uint8_t* a = (uint8_t*)d; while (n--) *a++ = 0; }
static int diff(const void* x, const void* y, size_t n) {
    const uint8_t* a = (const uint8_t*)x; const uint8_t* b = (const uint8_t*)y;
    uint8_t acc = 0;
    while (n--) acc |= (uint8_t)(*a++ ^ *b++);
    return acc != 0;
}

static int same(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static int side_name(char* out, size_t cap, const char* name, const char* ext) {
    size_t a = slen(name), b = slen(ext);
    if (a + 1 + b + 1 > cap) return 0;
    cpy(out, name, a); out[a] = '.'; cpy(out + a + 1, ext, b + 1);
    return 1;
}

/* Запись + образ из песочницы: размер, SHA-256 и разбор ELF сверяются с записью. Всё читается заново при каждом вызове. */
static int load_rec(const mbj_env_t* env, const char* name, void* buf, size_t cap, mbj_rec_t* rec, size_t* elen) {
    if (!env || !env->get || !env->put || !mbj_name_valid(name) || !buf) return MBJ_E_ARG;
    char rn[48];
    if (!side_name(rn, sizeof rn, name, "mbj")) return MBJ_E_ARG;
    int64_t n = env->get(env->ud, rn, rec, sizeof *rec);
    if (n != (int64_t)sizeof *rec) return MBJ_E_NOREC;
    if (rec->magic != MBJ_REC_MAGIC || rec->version != 1 || rec->state != MBJ_STATE_PENDING || rec->elf_len == 0) return MBJ_E_NOREC;
    if (rec->elf_len > cap) return MBJ_E_ELF_SIZE;
    int64_t e = env->get(env->ud, name, buf, cap);
    if (e < 0) return MBJ_E_NOREC;
    if ((uint64_t)e != rec->elf_len) return MBJ_E_TAMPER;
    uint8_t d[32];
    sha256(buf, (size_t)e, d);
    if (diff(d, rec->elf_sha, 32)) return MBJ_E_TAMPER;
    elf_info_t info;
    int er = elf_parse(buf, (size_t)e, env->win_lo, env->win_hi, env->max_pages, &info);
    if (er != ELF_OK) return MBJ_E_ELF_INVALID;
    *elen = (size_t)e;
    return MBJ_OK;
}

int mbj_inspect(const mbj_env_t* env, const char* name, void* buf, size_t cap, mbj_rec_t* rec, size_t* len) {
    return load_rec(env, name, buf, cap, rec, len);
}

static int write_grant(const mbj_env_t* env, const char* name, uint32_t kind, const mbj_rec_t* rec) {
    mbj_grant_t g;
    zero(&g, sizeof g);
    g.magic = MBJ_GRANT_MAGIC; g.kind = kind; g.elf_len = rec->elf_len;
    cpy(g.elf_sha, rec->elf_sha, 32);
    char gn[48];
    if (!side_name(gn, sizeof gn, name, "ok")) return MBJ_E_ARG;
    return env->put(env->ud, gn, &g, sizeof g) == 0 ? MBJ_OK : MBJ_E_IO;
}

int mbj_authorize_human(const mbj_env_t* env, const char* name, const char* typed_hex, void* buf, size_t cap) {
    mbj_rec_t rec;
    size_t n;
    int rc = load_rec(env, name, buf, cap, &rec, &n);
    if (rc) return rc;
    if (!typed_hex || !mbj_hex_prefix(typed_hex, rec.elf_sha)) return MBJ_E_CONFIRM;
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

static int read_grant(const mbj_env_t* env, const char* name, mbj_grant_t* g) {
    char gn[48];
    if (!side_name(gn, sizeof gn, name, "ok")) return 0;
    int64_t gl = env->get(env->ud, gn, g, sizeof *g);
    if (gl != (int64_t)sizeof *g) return 0;
    return g->magic == MBJ_GRANT_MAGIC && (g->kind == MBJ_GRANT_HUMAN || g->kind == MBJ_GRANT_SIGNED);
}

int mbj_grant_kind(const mbj_env_t* env, const char* name) {
    mbj_grant_t g;
    if (!env || !env->get || !mbj_name_valid(name)) return 0;
    return read_grant(env, name, &g) ? (int)g.kind : 0;
}

int mbj_check_grant(const mbj_env_t* env, const char* name, void* buf, size_t cap, size_t* len) {
    mbj_rec_t rec;
    size_t n;
    int rc = load_rec(env, name, buf, cap, &rec, &n);
    if (rc) return rc;
    mbj_grant_t g;
    if (!read_grant(env, name, &g)) return MBJ_E_NOGRANT;
    if (g.elf_len != rec.elf_len || diff(g.elf_sha, rec.elf_sha, 32)) return MBJ_E_TAMPER;
    if (len) *len = n;
    return MBJ_OK;
}

/* ---------------- таблица разрешений ядра ---------------- */

static int grant_slot_name(const char* side, char* out, size_t cap) {
    size_t k = slen(side);
    if (k < 4 || k >= cap || diff(side + k - 3, ".ok", 3)) return 0;
    cpy(out, side, k - 3); out[k - 3] = 0;
    return mbj_name_valid(out);
}

int mbj_grants_put(mbj_grants_t* t, const char* side, const void* data, size_t len) {
    char nm[48];
    mbj_grant_slot_t* free_slot = 0;
    if (!t || !side || !data || len != sizeof(mbj_grant_t) || !grant_slot_name(side, nm, sizeof nm)) return -1;
    const mbj_grant_t* g = (const mbj_grant_t*)data;
    if (g->magic != MBJ_GRANT_MAGIC || (g->kind != MBJ_GRANT_HUMAN && g->kind != MBJ_GRANT_SIGNED)) return -1;
    for (unsigned i = 0; i < MBJ_GRANT_SLOTS; i++) {
        mbj_grant_slot_t* s = &t->s[i];
        if (s->used && same(s->name, nm)) { s->g = *g; return 0; }
        if (!s->used && !free_slot) free_slot = s;
    }
    if (!free_slot) return -1;
    cpy(free_slot->name, nm, slen(nm) + 1);
    free_slot->g = *g; free_slot->used = 1;
    return 0;
}

int64_t mbj_grants_get(const mbj_grants_t* t, const char* side, void* buf, size_t cap) {
    char nm[48];
    if (!t || !buf || cap < sizeof(mbj_grant_t) || !grant_slot_name(side, nm, sizeof nm)) return -1;
    for (unsigned i = 0; i < MBJ_GRANT_SLOTS; i++) {
        if (t->s[i].used && same(t->s[i].name, nm)) { cpy(buf, &t->s[i].g, sizeof(mbj_grant_t)); return (int64_t)sizeof(mbj_grant_t); }
    }
    return -1;
}

void mbj_grants_drop(mbj_grants_t* t, const char* name) {
    if (!t || !name) return;
    for (unsigned i = 0; i < MBJ_GRANT_SLOTS; i++) {
        if (t->s[i].used && same(t->s[i].name, name)) zero(&t->s[i], sizeof t->s[i]);
    }
}

/* ---------------- темп и правило Разума ---------------- */

int mbj_rate_allow(const mbj_rate_t* r, uint32_t now_s) {
    if (r->pending >= r->max_pending) return 0;
    if (r->any && now_s - r->last_s < r->min_gap_s) return 0;
    return 1;
}
void mbj_rate_note_built(mbj_rate_t* r, uint32_t now_s) { r->last_s = now_s; r->any = 1; r->pending++; }
void mbj_rate_note_proposed(mbj_rate_t* r, uint32_t now_s) { r->last_s = now_s; r->any = 1; }
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
