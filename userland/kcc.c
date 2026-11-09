/* kcc на Кире: компилятор C как программа Ring 3. Запуск: cc <исходник> <результат> (аргументы - строка запуска).
   Второй режим: "@build probe N SEED" / "@build table N MUL MOD" - задание на сборку (mind_build.c): шаблон, ворота исходника, жёсткая
   арена и квоты, проверка ELF, запись образа и записи «ожидает» в песочницу /data/kcc. Режим ничего не запускает и ничего не
   разрешает (разрешение выдаёт только ядро, buildcmd.c, по команде человека); права процесса те же - только CAP_FS.
   Права: только CAP_FS. Файлы - песочница /data/kcc (короткие имена); заголовки libc-lite - /etc/kcc/include (файлы .h)
   (чтение через FS_OP_INC_*, доступное только этой программе). Память - арена поверх SYS_HEAP, растёт по требованию
   до предела, который назначает ядро (при отказе ядра компиляция заканчивается ошибкой «arena exhausted»). */
#include <stdio.h>
#include <string.h>
#include "kira.h"
#include "../kcc/kcc.h"
#include "../mind_build.h"

KIRA_ENTRY()

enum { KFS_INC_READ = 11, KFS_INC_STAT = 12 };
#define INC_DIR "/etc/kcc/include"
#define PAGE 4096ul
#define CHUNK_PAGES 256ul          /* арена растёт кусками не меньше 1 МБ */

static char *arena;
static size_t arena_cap;

static void say(const char *s) {
    size_t n = strlen(s);
    while (n) {                                     /* ядро принимает до 4096 байт за вызов */
        size_t k = n > 2048 ? 2048 : n;
        k_write(s, k);
        s += k; n -= k;
    }
}

static size_t grow(void *ud, size_t cap, size_t need) {
    (void)ud;
    unsigned long pages = (unsigned long)((need - cap + PAGE - 1) / PAGE), want = pages > CHUNK_PAGES ? pages : CHUNK_PAGES;
    void *p = k_heap_grow(want);
    if (!p && want > pages) { want = pages; p = k_heap_grow(want); }     /* лимит близко: берём ровно нужное */
    if (!p || (char *)p != arena + cap) return 0;                         /* куски кучи идут подряд; разрыв - отказ */
    return cap + want * PAGE;
}

/* Чтение файла для препроцессора: заголовки из /etc/kcc/include, остальное - песочница /data/kcc. */
static const char *rd(void *ud, const char *path, size_t *len) {
    (void)ud;
    int inc = 0;
    const char *n = path;
    if (!strncmp(path, INC_DIR "/", sizeof INC_DIR)) { n = path + sizeof INC_DIR; inc = 1; }
    else if (!strncmp(path, "/data/kcc/", 10)) n = path + 10;
    if (!*n || strchr(n, '/')) return 0;
    u64 nl = strlen(n);
    i64 sz = inc ? k_fs(KFS_INC_STAT, n, nl, 0, 0, 0) : k_file_size(n);
    if (sz < 0) return 0;
    char *buf = ka_new((size_t)sz + 1);
    if (sz) {
        i64 got = inc ? k_fs(KFS_INC_READ, n, nl, buf, (u64)sz, 0) : k_file_read(n, buf, (u64)sz, 0);
        if (got != sz) return 0;
    }
    *len = (size_t)sz;
    return buf;
}


/* ---- режим @build ---- */
static uint64_t b_now_ms(void *ud) { (void)ud; return k_clock_ns() / 1000000ull; }
static int b_put(void *ud, const char *name, const void *data, size_t len) { (void)ud; return k_file_write(name, data, len) >= 0 ? 0 : -1; }
static int64_t b_get(void *ud, const char *name, void *buf, size_t cap) {
    (void)ud;
    i64 sz = k_file_size(name);
    if (sz < 0) return -1;
    if ((u64)sz > cap) return -2;
    if (sz == 0) return 0;
    i64 got = k_file_read(name, buf, (u64)sz, 0);
    return got == sz ? got : -1;
}
static int b_num(char **q, u32 *v) {
    while (**q == ' ') (*q)++;
    if (**q < '0' || **q > '9') return 0;
    u64 x = 0;
    while (**q >= '0' && **q <= '9') { x = x * 10 + (u64)(**q - '0'); if (x > 0xFFFFFFFFull) return 0; (*q)++; }
    if (**q && **q != ' ') return 0;
    *v = (u32)x;
    return 1;
}

static int build_mode(char *args) {
    char *q = args + 6;                              /* после "@build" */
    mbj_job_t job;
    memset(&job, 0, sizeof job);
    while (*q == ' ') q++;
    if (!strncmp(q, "probe", 5) && (q[5] == ' ' || !q[5])) {
        job.tmpl = MBJ_T_PROBE; q += 5;
        if (!b_num(&q, &job.p[0]) || !b_num(&q, &job.p[1])) { say("использование: @build probe N SEED\n"); return 2; }
    } else if (!strncmp(q, "table", 5) && (q[5] == ' ' || !q[5])) {
        job.tmpl = MBJ_T_TABLE; q += 5;
        if (!b_num(&q, &job.p[0]) || !b_num(&q, &job.p[1]) || !b_num(&q, &job.p[2])) { say("использование: @build table N MUL MOD\n"); return 2; }
    } else { say("использование: @build probe N SEED | @build table N MUL MOD\n"); return 2; }
    while (*q == ' ') q++;
    if (*q) { say("[BUILD] лишние аргументы\n"); return 2; }
    void *mem = k_heap_grow(CHUNK_PAGES);            /* арена компилятора: ровно MBJ_DEF_ARENA, расти не может (grow = 0 в mind_build.c) */
    if (!mem) { say("[BUILD] нет памяти для арены компилятора\n"); return 2; }
    mbj_env_t env;
    memset(&env, 0, sizeof env);
    env.now_ms = b_now_ms; env.put = b_put; env.get = b_get;
    env.arena = mem; env.arena_cap = CHUNK_PAGES * PAGE;
#if defined(__aarch64__)
    env.target = "aarch64"; env.win_lo = 0x0000808000000000ull; env.win_hi = env.win_lo + 0x40000000ull;   /* окно программ ядра: arch/aarch64/arch_vmm.h */
#else
    env.target = "x86_64"; env.win_lo = 0x8000000000ull; env.win_hi = env.win_lo + 0x40000000ull;
#endif
    env.max_pages = 64;                              /* образ до 64 КБ: предел файла песочницы; решающую проверку с реальным пределом делает ядро */
    mbj_quota_t quota;
    memset(&quota, 0, sizeof quota);
    quota.max_elf = 64u << 10;
    mbj_result_t res;
    int rc = mbj_build(&env, &quota, &job, &res);
    char m[400];
    if (rc) {
        snprintf(m, sizeof m, "[BUILD] отказ: %s (%s)\n", mbj_strerror(rc), res.err);
        say(m);
        return 1;
    }
    /* строки журнала Киры короткие (USERLOG_LINE): отчёт - три строки */
    snprintf(m, sizeof m, "[BUILD] %s sha256 %s\n", res.name, res.elf_hex);
    say(m);
    snprintf(m, sizeof m, "[BUILD] %s размер %lu байт, лексем %u, арена %u КБ, сборка %u мс\n", res.name, (unsigned long)res.elf_len,
             (unsigned)res.tokens, (unsigned)(res.arena_used / 1024), (unsigned)res.ms);
    say(m);
    snprintf(m, sizeof m, "[BUILD] %s ожидает решения; запуска не было. Разрешить: build allow %s ХЕШ\n", res.name, res.name);
    say(m);
    return 0;
}

int umain(void) {
    char args[128];
    i64 al = k_args(args, sizeof args - 1);
    if (al < 0) al = 0;
    args[al] = 0;
    if (!strncmp(args, "@build", 6) && (args[6] == ' ' || !args[6])) k_exit(build_mode(args));
    char *wd[3] = { 0, 0, 0 };
    int nw = 0;
    for (char *q = args; *q;) {                    /* слова строки запуска; лишнее слово - ошибка использования */
        while (*q == ' ') q++;
        if (!*q) break;
        if (nw < 3) wd[nw] = q;
        nw++;
        while (*q && *q != ' ') q++;
        if (*q) *q++ = 0;
    }
    char *src = wd[0] ? wd[0] : (char *)"", *out = nw == 2 ? wd[1] : 0;
    if (!*src || !out || !*out) {
        say("использование: cc <исходник> <результат>   (файлы - в /data/kcc, имена короткие)\n");
        k_exit(2);
    }
    void *base = k_heap_grow(CHUNK_PAGES);
    if (!base) { say("kcc: нет памяти для арены компилятора\n"); k_exit(2); }
    arena = base; arena_cap = CHUNK_PAGES * PAGE;
    static const char *incs[1] = { INC_DIR };
    #if defined(__aarch64__)
    KCfg cfg = { rd, 0, incs, 1, 0, 0, "aarch64", grow };      /* программы собираются для той же архитектуры, на которой работает компилятор */
#else
    KCfg cfg = { rd, 0, incs, 1, 0, 0, "x86_64", grow };
#endif
    KResult r;
    int rc = kcc_compile(&cfg, arena, arena_cap, src, 2, &r);
    if (r.warn && *r.warn) say(r.warn);
    if (rc) { say(r.err); say("\n"); k_exit(1); }
    i64 w = k_file_write(out, r.elf, r.elflen);
    if (w < 0) {
        char m[96];
        snprintf(m, sizeof m, "kcc: не удалось записать '%s' (код %lld; имя: буквы, цифры, . - _)\n", out, (long long)w);
        say(m);
        k_exit(1);
    }
    char m[160];
    snprintf(m, sizeof m, "kcc: ok: %s -> %s, %lu байт (память компилятора %lu КБ)\n", src, out, (unsigned long)r.elflen, (unsigned long)(K.aused / 1024));
    say(m);
    return 0;
}
