// Хост-тест «задания на сборку» (mind_build.c): шаблон -> квоты -> настоящий kcc -> проверка ELF ядра -> песочница (каталог хоста),
// разрешение человека (хеш) и подписи (Ed25519, ключ KIRA_TEST), запуск ТОЛЬКО через mbj_check_grant под tests/kcc/kloader.
// Два этапа, потому что подпись делает настоящий офлайн-инструмент tools/kdrvsign.py (tests/mind_build_test.sh):
//   stage1 DIR            - всё, что не требует подписи; пишет DIR/cand.elf (образ-кандидат под подпись) и DIR/cand.name
//   stage2 DIR KLOADER    - подпись (верная и испорченная), запуск разрешённых образов под kloader и сверка с эталоном хоста
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mind_build.h"
#include "kdrv_trust.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static char SB[400];                       /* песочница: каталог хоста (на устройстве - /data/kcc) */
static uint64_t skew_ms, step_ms;          /* step_ms > 0: каждое чтение часов сдвигает их (проверка квоты времени) */
static int64_t utc_now = 1800000000;

static uint64_t t_now_ms(void* ud) {
    (void)ud;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    skew_ms += step_ms;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000 + skew_ms;
}
static int64_t t_now_utc(void* ud) { (void)ud; return utc_now; }
static int t_put(void* ud, const char* name, const void* data, size_t len) {
    (void)ud;
    if (strchr(name, '/')) return -1;
    char p[500]; snprintf(p, sizeof p, "%s/%s", SB, name);
    FILE* f = fopen(p, "wb"); if (!f) return -1;
    size_t w = fwrite(data, 1, len, f);
    return fclose(f) == 0 && w == len ? 0 : -1;
}
static int64_t t_get(void* ud, const char* name, void* buf, size_t cap) {
    (void)ud;
    if (strchr(name, '/')) return -1;
    char p[500]; snprintf(p, sizeof p, "%s/%s", SB, name);
    FILE* f = fopen(p, "rb"); if (!f) return -1;
    size_t n = fread(buf, 1, cap + 1, f);
    fclose(f);
    return n > cap ? -2 : (int64_t)n;
}
static int exists(const char* name) { char p[500]; snprintf(p, sizeof p, "%s/%s", SB, name); struct stat st; return stat(p, &st) == 0; }
static int sandbox_count(void) {
    char c[520]; snprintf(c, sizeof c, "ls %s | wc -l", SB);
    FILE* f = popen(c, "r"); int n = -1; if (f) { if (fscanf(f, "%d", &n) != 1) n = -1; pclose(f); }
    return n;
}

static char arena[4u << 20];
static mbj_env_t ENV;
static void env_init(void) {
    memset(&ENV, 0, sizeof ENV);
    ENV.now_ms = t_now_ms; ENV.now_utc = t_now_utc; ENV.put = t_put; ENV.get = t_get;
    ENV.arena = arena; ENV.arena_cap = sizeof arena; ENV.target = "x86_64";
    ENV.win_lo = 0x8000000000ULL; ENV.win_hi = 0x800000000000ULL; ENV.max_pages = 4096;
}

/* эталоны хоста для результатов образов (та же арифметика, что в шаблонах) */
static int ref_probe(uint32_t iters, uint32_t seed) {
    unsigned x = seed, acc = 0;
    for (unsigned i = 0; i < iters; i++) { x = x * 1664525u + 1013904223u; acc ^= x >> 7; acc += i; }
    return (int)(acc & 0x7fffffffu);
}
static int ref_table(uint32_t n, uint32_t mul, uint32_t mod) {
    unsigned s = 0;
    for (unsigned i = 0; i < n; i++) s += (unsigned)(((uint64_t)i * mul + 1) % mod) * (i + 1u);
    return (int)(s & 0x7fffffffu);
}

static mbj_job_t J_PROBE = { MBJ_T_PROBE, { 50000, 7, 0 } };
static mbj_job_t J_TABLE = { MBJ_T_TABLE, { 64, 37, 1009 } };

/* Запуск разрешён только через mbj_check_grant; образ пишется во временный файл из тех самых проверенных байт. */
static int run_if_granted(const char* name, const char* kloader, const char* dir, long* code) {
    static unsigned char buf[MBJ_HARD_ELF];
    size_t n = 0;
    int rc = mbj_check_grant(&ENV, name, buf, sizeof buf, &n);
    if (rc) return rc;
    char p[600]; snprintf(p, sizeof p, "%s/run.elf", dir);
    FILE* f = fopen(p, "wb"); if (!f) return MBJ_E_IO;
    fwrite(buf, 1, n, f); fclose(f);
    char c[900]; snprintf(c, sizeof c, "%s %s 2>&1", kloader, p);
    FILE* pp = popen(c, "r"); if (!pp) return MBJ_E_IO;
    char out[200] = { 0 };
    if (!fgets(out, sizeof out, pp)) out[0] = 0;
    pclose(pp);
    char* e = 0; *code = strtol(out, &e, 10);
    return e && e != out ? MBJ_OK : MBJ_E_IO;
}

static void stage1(const char* dir) {
    char c[500]; snprintf(c, sizeof c, "rm -rf %s/sb && mkdir -p %s/sb", dir, dir); if (system(c)) exit(2);
    snprintf(SB, sizeof SB, "%s/sb", dir);
    env_init();
    mbj_result_t r;
    mbj_quota_t q0; memset(&q0, 0, sizeof q0);

    /* ---- шаблоны: границы параметров ---- */
    char src[MBJ_HARD_SRC];
    mbj_job_t bad;
    bad = J_PROBE; bad.p[0] = 0;           CHECK(mbj_render(&bad, src, sizeof src) == MBJ_E_ARG);
    bad = J_PROBE; bad.p[0] = 4194305u;    CHECK(mbj_render(&bad, src, sizeof src) == MBJ_E_ARG);
    bad = J_TABLE; bad.p[0] = 0;           CHECK(mbj_render(&bad, src, sizeof src) == MBJ_E_ARG);
    bad = J_TABLE; bad.p[0] = 257;         CHECK(mbj_render(&bad, src, sizeof src) == MBJ_E_ARG);
    bad = J_TABLE; bad.p[2] = 0;           CHECK(mbj_render(&bad, src, sizeof src) == MBJ_E_ARG);
    bad = J_TABLE; bad.p[2] = 65537u;      CHECK(mbj_render(&bad, src, sizeof src) == MBJ_E_ARG);
    bad = J_PROBE; bad.tmpl = 9;           CHECK(mbj_render(&bad, src, sizeof src) == MBJ_E_ARG);
    CHECK(mbj_render(&J_PROBE, src, 20) == MBJ_E_SRC_SIZE);             /* буфер мал - ошибка, а не обрезка */
    CHECK(mbj_build(&ENV, &q0, &bad, &r) == MBJ_E_ARG && r.status == MBJ_E_ARG && sandbox_count() == 0);

    /* ---- успешные сборки: образ и запись «ожидает», разрешения нет, ничего не запускалось ---- */
    CHECK(mbj_build(&ENV, &q0, &J_PROBE, &r) == MBJ_OK);
    printf("probe: имя %s, образ %zu байт, лексем %u, исходник %u байт, арена %u байт, %u мс\n", r.name, r.elf_len, r.tokens, r.src_len, r.arena_used, r.ms);
    char probe[40]; snprintf(probe, sizeof probe, "%s", r.name);
    CHECK(exists(probe) && strlen(r.elf_hex) == 64 && r.elf_len > 0);
    char rn[60]; snprintf(rn, sizeof rn, "%s.mbj", probe); CHECK(exists(rn));
    char gn[60]; snprintf(gn, sizeof gn, "%s.ok", probe); CHECK(!exists(gn));
    static unsigned char buf[MBJ_HARD_ELF];
    size_t n = 0;
    CHECK(mbj_check_grant(&ENV, probe, buf, sizeof buf, &n) == MBJ_E_NOGRANT);          /* без решения запуск не допускается */
    char probe_hex[65]; memcpy(probe_hex, r.elf_hex, 65);

    CHECK(mbj_build(&ENV, &q0, &J_TABLE, &r) == MBJ_OK);
    printf("table: имя %s, образ %zu байт, лексем %u, исходник %u байт, арена %u байт, %u мс\n", r.name, r.elf_len, r.tokens, r.src_len, r.arena_used, r.ms);
    char table[40]; snprintf(table, sizeof table, "%s", r.name);
    char table_hex[65]; memcpy(table_hex, r.elf_hex, 65);
    CHECK(mbj_build(&ENV, &q0, &J_TABLE, &r) == MBJ_OK && !strcmp(r.name, table) && !memcmp(r.elf_hex, table_hex, 65));   /* сборка детерминирована */

    /* ---- квоты: чёткая ошибка, ничего не записано, нет падения ---- */
    int files = sandbox_count();
    mbj_quota_t q;
    memset(&q, 0, sizeof q); q.arena_bytes = 4096;
    CHECK(mbj_build(&ENV, &q, &J_PROBE, &r) == MBJ_E_ARENA && strstr(r.err, "arena exhausted"));
    memset(&q, 0, sizeof q); q.max_tokens = 20;
    CHECK(mbj_build(&ENV, &q, &J_PROBE, &r) == MBJ_E_TOKENS && r.tokens == 21);
    memset(&q, 0, sizeof q); q.max_src = 60;
    CHECK(mbj_build(&ENV, &q, &J_PROBE, &r) == MBJ_E_SRC_SIZE);
    memset(&q, 0, sizeof q); q.max_elf = 64;
    CHECK(mbj_build(&ENV, &q, &J_PROBE, &r) == MBJ_E_ELF_SIZE);
    memset(&q, 0, sizeof q); q.max_ms = 5; step_ms = 10;                                 /* часы шагают на 10 мс за чтение: «сборка» дольше 5 мс */
    CHECK(mbj_build(&ENV, &q, &J_PROBE, &r) == MBJ_E_TIME);
    step_ms = 0;
    memset(&q, 0, sizeof q); q.arena_bytes = 1u << 30;  CHECK(mbj_build(&ENV, &q, &J_PROBE, &r) == MBJ_E_QUOTA);
    memset(&q, 0, sizeof q); q.max_ms = 100000;         CHECK(mbj_build(&ENV, &q, &J_PROBE, &r) == MBJ_E_QUOTA);
    memset(&q, 0, sizeof q); q.max_tokens = 100000;     CHECK(mbj_build(&ENV, &q, &J_PROBE, &r) == MBJ_E_QUOTA);
    CHECK(sandbox_count() == files);                                                      /* отказ квоты ничего не записывает */
    {   /* несколько больше арена, чем нужно: тот же образ (квота арены не влияет на результат) */
        memset(&q, 0, sizeof q); q.arena_bytes = 2u << 20;
        CHECK(mbj_build(&ENV, &q, &J_TABLE, &r) == MBJ_OK && !memcmp(r.elf_hex, table_hex, 65));
    }

    /* ---- неверный и опасный исходник: ошибка, а не падение ---- */
    struct { const char* src; int want; const char* part; } bs[] = {
        { "int main( {", MBJ_E_COMPILE, "error" },
        { "int main(void) { return foo(); }", MBJ_E_COMPILE, 0 },
        { "int main(void) { int a[n]; return 0; }", MBJ_E_COMPILE, 0 },
        { "int main(void) { return 1 +; }", MBJ_E_COMPILE, "error" },
        { "#include <stdio.h>\nint main(void){return 0;}", MBJ_E_SRC_FORBIDDEN, "препроцессор" },
        { "int main(void){ __asm__(\"int3\"); return 0; }", MBJ_E_SRC_FORBIDDEN, "asm" },
        { "int main(void){ asm(\"int3\"); return 0; }", MBJ_E_SRC_FORBIDDEN, "asm" },
        { "int main(void){ return '?' ; /* x", MBJ_E_SRC_FORBIDDEN, "комментарий" },
        { "int main(void){ return \"abc; }", MBJ_E_SRC_FORBIDDEN, "литерал" },
        { "int main(void){ return 0; }\xff", MBJ_E_SRC_FORBIDDEN, "ASCII" },
        { "int main(void){ return 0; } ?\?=", MBJ_E_SRC_FORBIDDEN, "триграф" },
        { "int main(void){ return 0; } %:", MBJ_E_SRC_FORBIDDEN, "диграф" },
        { "int main(void){ return 0; } \\", MBJ_E_SRC_FORBIDDEN, "косая" },
        { "", MBJ_E_SRC_FORBIDDEN, "пустой" },
    };
    for (unsigned i = 0; i < sizeof bs / sizeof bs[0]; i++) {
        int rc = mbj_build_source(&ENV, &q0, 9, bs[i].src, strlen(bs[i].src), &r);
        if (rc != bs[i].want || r.status != rc || !r.err[0] || (bs[i].part && !strstr(r.err, bs[i].part))) {
            printf("плохой исходник #%u: rc=%d err='%s'\n", i, rc, r.err);
            CHECK(0);
        }
    }
    CHECK(sandbox_count() == files);

    /* ---- устойчивость: повреждённые копии верного исходника (детерминированно) не роняют сборку ---- */
    {
        int n0 = mbj_render(&J_TABLE, src, sizeof src);
        CHECK(n0 > 0);
        uint64_t st = 0x9E3779B97F4A7C15ull;
        unsigned ok = 0, rejected = 0;
        for (int it = 0; it < 400; it++) {
            char m[MBJ_HARD_SRC]; memcpy(m, src, (size_t)n0);
            for (int k = 0; k < 1 + it % 4; k++) {
                st ^= st << 13; st ^= st >> 7; st ^= st << 17;
                m[st % (uint64_t)n0] = (char)(" ;{}()[]*&,=+-<>!~0aZ_\"'/#\\\n"[(st >> 20) % 29]);
            }
            int rc = mbj_build_source(&ENV, &q0, 9, m, (size_t)n0, &r);
            CHECK(rc == r.status && (rc == MBJ_OK || (rc < 0 && r.err[0])));
            if (rc == MBJ_OK) ok++; else rejected++;
        }
        printf("повреждённый исходник: 400 попыток, собрано %u, отвергнуто с ошибкой %u, падений 0\n", ok, rejected);
        CHECK(rejected > 0);
        /* sandbox теперь содержит и мусорные удачные сборки (они валидны, но без решения); чистим для дальнейших проверок */
        snprintf(c, sizeof c, "find %s -name 'mb-t9-*' -delete", SB); if (system(c)) exit(2);
    }

    /* ---- решение человека ---- */
    CHECK(mbj_authorize_human(&ENV, probe, "deadbeef", buf, sizeof buf) == MBJ_E_CONFIRM);
    CHECK(mbj_authorize_human(&ENV, probe, "", buf, sizeof buf) == MBJ_E_CONFIRM);
    { char shortx[8]; memcpy(shortx, probe_hex, 7); shortx[7] = 0; CHECK(mbj_authorize_human(&ENV, probe, shortx, buf, sizeof buf) == MBJ_E_CONFIRM); }   /* < 8 знаков */
    { char other[9]; memcpy(other, table_hex, 8); other[8] = 0; CHECK(mbj_authorize_human(&ENV, probe, other, buf, sizeof buf) == MBJ_E_CONFIRM); }       /* хеш чужого образа */
    CHECK(mbj_authorize_human(&ENV, "../probe", probe_hex, buf, sizeof buf) == MBJ_E_ARG);
    CHECK(mbj_authorize_human(&ENV, "mb-nonexistent", probe_hex, buf, sizeof buf) == MBJ_E_NOREC);
    CHECK(!exists(gn) && mbj_check_grant(&ENV, probe, buf, sizeof buf, &n) == MBJ_E_NOGRANT);
    { char pre[17]; memcpy(pre, probe_hex, 16); pre[16] = 0; CHECK(mbj_authorize_human(&ENV, probe, pre, buf, sizeof buf) == MBJ_OK); }
    CHECK(exists(gn) && mbj_check_grant(&ENV, probe, buf, sizeof buf, &n) == MBJ_OK && n > 0);
    {   /* подмена образа после разрешения: байт изменён -> разрешение больше не действует */
        char p[500]; snprintf(p, sizeof p, "%s/%s", SB, probe);
        FILE* f = fopen(p, "r+b"); CHECK(f != 0);
        if (f) { fseek(f, 100, SEEK_SET); int ch = fgetc(f); fseek(f, 100, SEEK_SET); fputc(ch ^ 0x55, f); fclose(f); }
        CHECK(mbj_check_grant(&ENV, probe, buf, sizeof buf, &n) == MBJ_E_TAMPER);
        CHECK(mbj_authorize_human(&ENV, probe, probe_hex, buf, sizeof buf) == MBJ_E_TAMPER);
        /* пересобрать тот же образ: байты вернулись, разрешение снова сходится (оно привязано к хешу, а не к имени) */
        CHECK(mbj_build(&ENV, &q0, &J_PROBE, &r) == MBJ_OK && mbj_check_grant(&ENV, probe, buf, sizeof buf, &n) == MBJ_OK);
    }

    /* ---- темп заданий и правило Разума ---- */
    {
        mbj_rate_t rt = { 0, 0, 600, 2, 0 };
        CHECK(mbj_rate_allow(&rt, 100));
        mbj_rate_note_built(&rt, 100);
        CHECK(!mbj_rate_allow(&rt, 200) && !mbj_rate_allow(&rt, 699) && mbj_rate_allow(&rt, 700));
        mbj_rate_note_built(&rt, 700);
        CHECK(!mbj_rate_allow(&rt, 5000));                                  /* два ожидающих решения - больше не строим */
        mbj_rate_note_decided(&rt);
        CHECK(mbj_rate_allow(&rt, 5000));

        static mind_t M; static mind_sample_t S;
        mind_init(&M, 0, 4);
        memset(&S, 0, sizeof S); S.t = 500; S.tsc_khz = 3000000; S.cores = 4;
        M.health = 95; M.calm_s = 100; M.late_s = 0;
        mbj_rate_t fresh = { 0, 0, 600, 2, 0 };
        mbj_job_t pj;
        CHECK(mind_build_propose(&M, &S, 500, &fresh, &pj) == 1 && pj.tmpl == MBJ_T_PROBE && pj.p[0] == 2500000u);
        CHECK(mbj_build(&ENV, &q0, &pj, &r) == MBJ_OK);                      /* предложенное проходит те же ворота */
        M.late_s = 3; CHECK(mind_build_propose(&M, &S, 500, &fresh, &pj) == 0); M.late_s = 0;      /* экран опаздывает */
        M.health = 60; CHECK(mind_build_propose(&M, &S, 500, &fresh, &pj) == 0); M.health = 95;
        M.calm_s = 5; CHECK(mind_build_propose(&M, &S, 500, &fresh, &pj) == 0); M.calm_s = 100;
        M.m.mode = MIND_OBSERVE; CHECK(mind_build_propose(&M, &S, 500, &fresh, &pj) == 0); M.m.mode = MIND_ACT;
        S.tsc_khz = 0; CHECK(mind_build_propose(&M, &S, 500, &fresh, &pj) == 0); S.tsc_khz = 3000000;
        mbj_rate_t busy = { 450, 1, 600, 2, 1 }; CHECK(mind_build_propose(&M, &S, 500, &busy, &pj) == 0);
        S.tsc_khz = 1; CHECK(mind_build_propose(&M, &S, 500, &fresh, &pj) == 1 && pj.p[0] == 1000u);   /* нижняя граница */
        S.tsc_khz = 0xFFFFFFFFu; CHECK(mind_build_propose(&M, &S, 500, &fresh, &pj) == 1 && pj.p[0] == 4194304u && mbj_render(&pj, src, sizeof src) > 0);
    }

    /* ---- кандидат под подпись (второй этап) и таблица для запуска ---- */
    {
        CHECK(mbj_authorize_human(&ENV, table, table_hex, buf, sizeof buf) == MBJ_OK);      /* TABLE разрешён человеком; PROBE пойдёт по подписи */
        snprintf(gn, sizeof gn, "%s.ok", probe);
        char p[500]; snprintf(p, sizeof p, "%s/%s", SB, gn); remove(p);                      /* PROBE: человеческое решение снято, ждёт подписи */
        CHECK(mbj_check_grant(&ENV, probe, buf, sizeof buf, &n) == MBJ_E_NOGRANT);
        FILE* f;
        snprintf(p, sizeof p, "%s/cand.name", dir); f = fopen(p, "w"); fprintf(f, "%s\n%s\n", probe, table); fclose(f);
        snprintf(p, sizeof p, "%s/cand.elf", dir); f = fopen(p, "wb");
        int64_t e = t_get(0, probe, buf, sizeof buf); fwrite(buf, 1, (size_t)e, f); fclose(f);
    }
}

static void stage2(const char* dir, const char* kloader) {
    snprintf(SB, sizeof SB, "%s/sb", dir);
    env_init();
    static unsigned char buf[MBJ_HARD_ELF];
    char probe[40], table[40], p[600];
    snprintf(p, sizeof p, "%s/cand.name", dir);
    FILE* f = fopen(p, "r"); CHECK(f != 0); if (!f) return;
    if (fscanf(f, "%39s %39s", probe, table) != 2) { printf("cand.name\n"); exit(2); }
    fclose(f);
    unsigned char sig[sizeof(kdrv_sig_t)];
    snprintf(p, sizeof p, "%s/cand.sig", dir); f = fopen(p, "rb"); CHECK(f != 0); if (!f) return;
    CHECK(fread(sig, 1, sizeof sig, f) == sizeof sig); fclose(f);
    size_t n;
    long code = -1;

    /* без решения не запускается */
    CHECK(run_if_granted(probe, kloader, dir, &code) == MBJ_E_NOGRANT && code == -1);

    /* подпись: порча подписи, чужой ключ, срок, другой образ - отказ; запись о разрешении не появляется */
    unsigned char bad[sizeof sig];
    memcpy(bad, sig, sizeof sig); bad[sizeof sig - 1] ^= 1;
    CHECK(mbj_authorize_signed(&ENV, probe, bad, sizeof bad, kdrv_trust_keys, KDRV_TRUST_KEY_COUNT, buf, sizeof buf) == MBJ_E_SIG);
    memcpy(bad, sig, sizeof sig); bad[8] ^= 1;                                          /* key_id */
    CHECK(mbj_authorize_signed(&ENV, probe, bad, sizeof bad, kdrv_trust_keys, KDRV_TRUST_KEY_COUNT, buf, sizeof buf) == MBJ_E_SIG);
    CHECK(mbj_authorize_signed(&ENV, probe, sig, sizeof sig - 1, kdrv_trust_keys, KDRV_TRUST_KEY_COUNT, buf, sizeof buf) == MBJ_E_SIG);   /* короткая */
    CHECK(mbj_authorize_signed(&ENV, table, sig, sizeof sig, kdrv_trust_keys, KDRV_TRUST_KEY_COUNT, buf, sizeof buf) == MBJ_E_SIG);        /* подпись другого образа */
    kdrv_sig_key_t revoked = kdrv_trust_keys[0]; revoked.flags = KDRV_SIG_KEY_REVOKED;
    CHECK(mbj_authorize_signed(&ENV, probe, sig, sizeof sig, &revoked, 1, buf, sizeof buf) == MBJ_E_SIG);                                   /* ключ отозван */
    kdrv_sig_key_t other = kdrv_trust_keys[0]; other.public_key[0] ^= 1;
    CHECK(mbj_authorize_signed(&ENV, probe, sig, sizeof sig, &other, 1, buf, sizeof buf) == MBJ_E_SIG);                                     /* неверный ключ */
    utc_now = 4200000000LL;
    CHECK(mbj_authorize_signed(&ENV, probe, sig, sizeof sig, kdrv_trust_keys, KDRV_TRUST_KEY_COUNT, buf, sizeof buf) == MBJ_E_SIG);        /* срок вышел */
    utc_now = 1800000000;
    CHECK(mbj_authorize_signed(&ENV, probe, sig, sizeof sig, 0, 0, buf, sizeof buf) == MBJ_E_ARG);
    char gn[60]; snprintf(gn, sizeof gn, "%s.ok", probe);
    CHECK(!(snprintf(p, sizeof p, "%s/%s", SB, gn), access(p, 0) == 0));
    CHECK(run_if_granted(probe, kloader, dir, &code) == MBJ_E_NOGRANT && code == -1);

    /* верная подпись: разрешение, затем запуск под kloader и сверка с эталоном хоста */
    CHECK(mbj_authorize_signed(&ENV, probe, sig, sizeof sig, kdrv_trust_keys, KDRV_TRUST_KEY_COUNT, buf, sizeof buf) == MBJ_OK);
    CHECK(run_if_granted(probe, kloader, dir, &code) == MBJ_OK);
    int want = ref_probe(J_PROBE.p[0], J_PROBE.p[1]);
    printf("probe под kloader: образ дал %ld, эталон хоста %d\n", code, want);
    CHECK(code == want);

    /* TABLE разрешён человеком на этапе 1 */
    CHECK(run_if_granted(table, kloader, dir, &code) == MBJ_OK);
    want = ref_table(J_TABLE.p[0], J_TABLE.p[1], J_TABLE.p[2]);
    printf("table под kloader: образ дал %ld, эталон хоста %d\n", code, want);
    CHECK(code == want);

    /* ELF, портящий проверку ядра, не доходит до песочницы: обрезанный образ записан извне -> check_grant видит несовпадение */
    {
        int64_t e = t_get(0, table, buf, sizeof buf);
        CHECK(e > 100 && t_put(0, table, buf, (size_t)e - 1) == 0);
        CHECK(mbj_check_grant(&ENV, table, buf, sizeof buf, &n) == MBJ_E_TAMPER);
    }
}

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "stage1")) stage1(argv[2]);
    else if (argc >= 4 && !strcmp(argv[1], "stage2")) stage2(argv[2], argv[3]);
    else { fprintf(stderr, "usage: mind_build_test stage1 DIR | stage2 DIR KLOADER\n"); return 2; }
    if (fails) printf("MIND BUILD TESTS FAILED (%d)\n", fails);
    else printf("mind build %s: ok\n", argv[1]);
    return fails != 0;
}
