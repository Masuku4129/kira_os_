/* kcc на Кире: компилятор C как программа Ring 3. Запуск: cc <исходник> <результат> (аргументы - строка запуска).
   Права: только CAP_FS. Файлы - песочница /data/kcc (короткие имена); заголовки libc-lite - /etc/kcc/include (файлы .h)
   (чтение через FS_OP_INC_*, доступное только этой программе). Память - арена поверх SYS_HEAP, растёт по требованию
   до предела, который назначает ядро (при отказе ядра компиляция заканчивается ошибкой «arena exhausted»). */
#include <stdio.h>
#include <string.h>
#include "kira.h"
#include "../kcc/kcc.h"

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

int umain(void) {
    char args[128];
    i64 al = k_args(args, sizeof args - 1);
    if (al < 0) al = 0;
    args[al] = 0;
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
