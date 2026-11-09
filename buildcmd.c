/* Консольная команда `build`: задание на сборку (mind_build.*), решение человека и запуск без прав. См. buildcmd.h и docs/BUILDER_PLAN.md.

   Разделение прав:
     - сборка идёт в процессе kcc (только CAP_FS, режим @build в userland/kcc.c); он пишет образ и запись в свою песочницу /data/kcc;
     - ядро в песочницу НЕ пишет и разрешения там не хранит: разрешения лежат в таблице ядра (mbj_grants_t), недоступной процессу kcc;
     - allow и run выполняются только отсюда, из команды человека (console.c); Разум получает лишь build_mind_tick (предложение);
     - запуск берёт байты, которые вернул mbj_check_grant, и не читает файл повторно (prog_run_local_image), права = 0. */
#include "buildcmd.h"
#include "mind_build_grant.h"
#include "kdrv_trust.h"
#include "kfs.h"
#include "heap.h"
#include "uart.h"
#include "progs.h"
#include "proc.h"
#include "rtc.h"
#include "spinlock.h"

#define SBX "/data/kcc/"
#define MAX_LIST 16
#define MAX_PENDING_HUMAN 8

static mbj_grants_t grants;                              /* только основной поток консоли */
static spinlock_t note_lock = SPINLOCK_INIT;
static mbj_rate_t rate = { 0, 0, 600, 4, 0 };            /* Разум: не чаще одного предложения за 10 минут, не больше 4 ожидающих решения */
static struct { uint8_t have; mbj_job_t job; uint32_t at_s; } last_prop;

static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static int same(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* Имя файла песочницы -> путь VFS. Допустимы только [A-Za-z0-9._-], не с точки, до 40 знаков: никаких '/', '..' и подобного. */
static int sbx_path(char* out, size_t cap, const char* name) {
    size_t k = slen(name);
    if (!k || k > 40 || name[0] == '.' || sizeof SBX + k > cap) return 0;
    for (size_t i = 0; i < k; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return 0;
    }
    size_t n = 0;
    for (const char* p = SBX; *p; p++) out[n++] = *p;
    for (size_t i = 0; i < k; i++) out[n++] = name[i];
    out[n] = 0;
    return 1;
}

static int has_ok_suffix(const char* n) { size_t k = slen(n); return k > 3 && same(n + k - 3, ".ok"); }

/* Окружение mbj для ядра. put принимает только разрешения (".ok" -> таблица ядра); в песочницу ядро не пишет. get читает образ и запись
   из песочницы (kfs) и разрешения из таблицы. */
static int env_put(void* ud, const char* name, const void* data, size_t len) {
    (void)ud;
    return has_ok_suffix(name) ? mbj_grants_put(&grants, name, data, len) : -1;
}
static int64_t env_get(void* ud, const char* name, void* buf, size_t cap) {
    (void)ud;
    if (has_ok_suffix(name)) return mbj_grants_get(&grants, name, buf, cap);
    char path[96];
    if (!sbx_path(path, sizeof path, name)) return -1;
    kfs_stat_t st;
    if (kfs_stat(path, &st) != 0 || st.type != KFS_FILE || st.size > cap) return -2;
    int64_t got = kfs_read(path, 0, buf, (size_t)st.size);
    return got == (int64_t)st.size ? got : -1;
}
static int64_t env_utc(void* ud) {
    (void)ud;
    rtc_time_t t;
    return rtc_now(0, &t) == 0 ? rtc_to_epoch(&t) : 0;
}
static void env_make(mbj_env_t* e) {
    for (unsigned i = 0; i < sizeof *e; i++) ((uint8_t*)e)[i] = 0;
    e->now_utc = env_utc; e->put = env_put; e->get = env_get;
    proc_user_window(&e->win_lo, &e->win_hi, &e->max_pages);
}

/* ---- разбор ---- */
static const char* skip(const char* s) { while (*s == ' ') s++; return s; }
static unsigned wlen(const char* s) { unsigned n = 0; while (s[n] && s[n] != ' ') n++; return n; }
static int word(const char** s, char* out, unsigned cap) {
    *s = skip(*s);
    unsigned n = wlen(*s);
    if (!n || n >= cap) return 0;
    for (unsigned i = 0; i < n; i++) out[i] = (*s)[i];
    out[n] = 0;
    *s += n;
    return 1;
}
static int num(const char** s, uint32_t* v) {
    char w[16];
    if (!word(s, w, sizeof w)) return 0;
    uint64_t x = 0;
    for (unsigned i = 0; w[i]; i++) {
        if (w[i] < '0' || w[i] > '9') return 0;
        x = x * 10 + (uint64_t)(w[i] - '0');
        if (x > 0xFFFFFFFFull) return 0;
    }
    *v = (uint32_t)x;
    return 1;
}

/* ---- список песочницы ---- */
typedef struct { char name[MAX_LIST][40]; unsigned n, total; } names_t;
static int collect(void* c, const char* nm, const kfs_stat_t* st) {
    names_t* l = (names_t*)c;
    size_t k = slen(nm);
    if (st->type != KFS_FILE || k < 8 || k > 36 || !same(nm + k - 4, ".mbj")) return 0;
    char base[40];
    for (size_t i = 0; i + 4 < k; i++) base[i] = nm[i];
    base[k - 4] = 0;
    if (!mbj_name_valid(base)) return 0;
    l->total++;
    if (l->n < MAX_LIST) { for (size_t i = 0; i <= k - 4; i++) l->name[l->n][i] = base[i]; l->n++; }
    return 0;
}
static int scan(names_t* l) {
    l->n = 0; l->total = 0;
    int r = kfs_list("/data/kcc", collect, l);
    return r == KFS_ERR_NOENT ? 0 : r;
}

static uint32_t count_pending(void) {
    names_t* l = (names_t*)kmalloc(sizeof *l);
    if (!l) return 0;
    uint32_t pend = 0;
    mbj_env_t env;
    env_make(&env);
    uint8_t* buf = (uint8_t*)kmalloc(MBJ_HARD_ELF);
    if (buf && scan(l) == 0) {
        for (unsigned i = 0; i < l->n; i++) if (!mbj_grant_kind(&env, l->name[i])) pend++;
    }
    if (buf) kfree(buf);
    kfree(l);
    uint64_t f = spin_lock_irqsave(&note_lock);
    rate.pending = pend;
    spin_unlock_irqrestore(&note_lock, f);
    return pend;
}

static void refuse(const char* name, int rc) {
    uart_print("[BUILD] "); uart_print(name); uart_print(": отказ: "); uart_print(mbj_strerror(rc)); uart_print("\n");
}

static void usage(void) {
    uart_print("использование:\n"
               "  build probe N SEED       собрать зонд-цикл (N итераций 1..4194304) процессом kcc без прав\n"
               "  build table N MUL MOD    собрать таблицу (N записей 1..256, MOD 1..65536)\n"
               "  build list               образы в песочнице /data/kcc: SHA-256, размер, решение\n"
               "  build allow ИМЯ ХЕШ      разрешить: первые 8+ знаков SHA-256 образа из отчёта (build list)\n"
               "  build allow ИМЯ sig      разрешить по подписи издателя из /data/kcc/ИМЯ.sig (ключи kdrv_trust_keys)\n"
               "  build run ИМЯ            запустить разрешённый образ БЕЗ прав; образ перепроверяется перед запуском\n"
               "  build drop ИМЯ           удалить образ, запись и разрешение\n"
               "Сборка сама ничего не запускает; Разум только предлагает задание, решает человек.\n");
}

static void show_proposal(void) {
    mbj_job_t j; uint32_t at; uint8_t have;
    uint64_t f = spin_lock_irqsave(&note_lock);
    have = last_prop.have; j = last_prop.job; at = last_prop.at_s;
    spin_unlock_irqrestore(&note_lock, f);
    if (!have) return;
    uart_print("Разум предлагал (t="); uart_print_dec(at); uart_print(" с): build probe "); uart_print_dec(j.p[0]); uart_print(" "); uart_print_dec(j.p[1]); uart_print("\n");
}

static void cmd_new(const char* verb, const char* a) {
    mbj_job_t job;
    job.tmpl = 0; job.p[0] = job.p[1] = job.p[2] = 0;
    char args[64]; unsigned n = 0;
    if (same(verb, "probe")) {
        job.tmpl = MBJ_T_PROBE;
        if (!num(&a, &job.p[0]) || !num(&a, &job.p[1]) || *skip(a)) { uart_print("использование: build probe N SEED\n"); return; }
    } else {
        job.tmpl = MBJ_T_TABLE;
        if (!num(&a, &job.p[0]) || !num(&a, &job.p[1]) || !num(&a, &job.p[2]) || *skip(a)) { uart_print("использование: build table N MUL MOD\n"); return; }
    }
    int vr = mbj_job_valid(&job);
    if (vr) { refuse("задание", vr); return; }
    if (!kfs_mounted()) { uart_print("[BUILD] нет файловой системы\n"); return; }
    if (count_pending() >= MAX_PENDING_HUMAN) { refuse("задание", MBJ_E_RATE); uart_print("  освободите место: build drop ИМЯ\n"); return; }
    const char* pre = same(verb, "probe") ? "@build probe " : "@build table ";
    for (const char* p = pre; *p; p++) args[n++] = *p;
    uint32_t v[3] = { job.p[0], job.p[1], job.p[2] };
    for (unsigned i = 0; i < (job.tmpl == MBJ_T_PROBE ? 2u : 3u); i++) {
        char t[11]; unsigned k = 0;
        uint32_t x = v[i];
        do { t[k++] = (char)('0' + x % 10); x /= 10; } while (x);
        while (k) args[n++] = t[--k];
        args[n++] = ' ';
    }
    args[n - 1] = 0;
    int pid = prog_run_args("kcc", args);
    if (pid < 0) { uart_print("[BUILD] не удалось запустить процесс сборки kcc (код "); uart_print_dec((uint64_t)(-pid)); uart_print(")\n"); return; }
    uart_print("[BUILD] задание принято, сборка идёт в процессе kcc (pid "); uart_print_dec((uint64_t)pid);
    uart_print(", только CAP_FS); отчёт появится строкой \"[BUILD] ... sha256 ...\" или в build list. Ничего не запускается.\n");
}

static void cmd_list(void) {
    if (!kfs_mounted()) { uart_print("[BUILD] нет файловой системы\n"); return; }
    names_t* l = (names_t*)kmalloc(sizeof *l);
    uint8_t* buf = (uint8_t*)kmalloc(MBJ_HARD_ELF);
    if (!l || !buf) { uart_print("[BUILD] нет памяти\n"); if (l) kfree(l); if (buf) kfree(buf); return; }
    int r = scan(l);
    if (r) { uart_print("[BUILD] /data/kcc: "); uart_print(kfs_strerror(r)); uart_print("\n"); }
    else if (!l->total) uart_print("[BUILD] образов нет\n");
    mbj_env_t env;
    env_make(&env);
    uint32_t pend = 0;
    for (unsigned i = 0; i < l->n; i++) {
        mbj_rec_t rec; size_t len = 0;
        int rc = mbj_inspect(&env, l->name[i], buf, MBJ_HARD_ELF, &rec, &len);
        uart_print("  "); uart_print(l->name[i]);
        if (rc) { uart_print("  не годен: "); uart_print(mbj_strerror(rc)); uart_print("\n"); continue; }
        char hx[65];
        mbj_hex(rec.elf_sha, 32, hx);
        uart_print("  шаблон "); uart_print_dec(rec.tmpl); uart_print("  "); uart_print_dec(len); uart_print(" байт  sha256 "); uart_print(hx);
        int kind = mbj_grant_kind(&env, l->name[i]);
        if (kind == MBJ_GRANT_HUMAN) uart_print("  разрешён человеком\n");
        else if (kind == MBJ_GRANT_SIGNED) uart_print("  разрешён подписью\n");
        else { pend++; uart_print("  ожидает решения\n"); }
    }
    if (l->total > l->n) { uart_print("  ... и ещё "); uart_print_dec(l->total - l->n); uart_print("\n"); }
    uint64_t f = spin_lock_irqsave(&note_lock);
    if (!r && l->total <= l->n) rate.pending = pend;
    spin_unlock_irqrestore(&note_lock, f);
    show_proposal();
    kfree(buf); kfree(l);
}

static void cmd_allow(const char* a) {
    char name[40], what[72];
    if (!word(&a, name, sizeof name) || !word(&a, what, sizeof what) || *skip(a)) { uart_print("использование: build allow ИМЯ ХЕШ (8+ знаков SHA-256 из build list) | build allow ИМЯ sig\n"); return; }
    uint8_t* buf = (uint8_t*)kmalloc(MBJ_HARD_ELF);
    if (!buf) { uart_print("[BUILD] нет памяти\n"); return; }
    mbj_env_t env;
    env_make(&env);
    int rc;
    const char* how;
    if (same(what, "sig")) {
        how = "подписью издателя";
        kdrv_sig_t sig;
        char sn[48]; unsigned k = 0;
        for (const char* p = name; *p && k < 40; p++) sn[k++] = *p;
        for (const char* p = ".sig"; *p; p++) sn[k++] = *p;
        sn[k] = 0;
        char path[96];
        kfs_stat_t st;
        if (!mbj_name_valid(name)) rc = MBJ_E_ARG;
        else if (!sbx_path(path, sizeof path, sn) || kfs_stat(path, &st) != 0 || st.type != KFS_FILE || st.size != sizeof sig) rc = MBJ_E_SIG;
        else if (kfs_read(path, 0, &sig, sizeof sig) != (int64_t)sizeof sig) rc = MBJ_E_IO;
        else rc = mbj_authorize_signed(&env, name, &sig, sizeof sig, kdrv_trust_keys, KDRV_TRUST_KEY_COUNT, buf, MBJ_HARD_ELF);
    } else {
        how = "человеком";
        rc = mbj_authorize_human(&env, name, what, buf, MBJ_HARD_ELF);
    }
    kfree(buf);
    if (rc) { refuse(name, rc); return; }
    (void)count_pending();
    uart_print("[BUILD] "); uart_print(name); uart_print(" разрешён "); uart_print(how); uart_print(": разрешение действует на эти байты образа; запуск: build run ");
    uart_print(name); uart_print("\n");
}

static void cmd_run(const char* a) {
    char name[40];
    if (!word(&a, name, sizeof name) || *skip(a)) { uart_print("использование: build run ИМЯ\n"); return; }
    uint8_t* buf = (uint8_t*)kmalloc(MBJ_HARD_ELF);
    if (!buf) { uart_print("[BUILD] нет памяти\n"); return; }
    mbj_env_t env;
    env_make(&env);
    size_t len = 0;
    int rc = mbj_check_grant(&env, name, buf, MBJ_HARD_ELF, &len);
    if (rc) { kfree(buf); refuse(name, rc); return; }
    int pid = prog_run_local_image(name, buf, len);       /* те самые проверенные байты; права 0 */
    kfree(buf);
    if (pid < 0) { uart_print("[BUILD] "); uart_print(name); uart_print(": запуск не удался (код "); uart_print_dec((uint64_t)(-pid)); uart_print(")\n"); return; }
    uart_print("[BUILD] "); uart_print(name); uart_print(" запущен без прав (pid "); uart_print_dec((uint64_t)pid); uart_print(")\n");
}

static void cmd_drop(const char* a) {
    char name[40];
    if (!word(&a, name, sizeof name) || *skip(a)) { uart_print("использование: build drop ИМЯ\n"); return; }
    if (!mbj_name_valid(name)) { refuse(name, MBJ_E_ARG); return; }
    char rn[48]; unsigned k = 0;
    for (const char* p = name; *p; p++) rn[k++] = *p;
    for (const char* p = ".mbj"; *p; p++) rn[k++] = *p;
    rn[k] = 0;
    char p1[96], p2[96];
    if (!sbx_path(p1, sizeof p1, name) || !sbx_path(p2, sizeof p2, rn)) { refuse(name, MBJ_E_ARG); return; }
    int r1 = kfs_unlink(p1), r2 = kfs_unlink(p2);
    mbj_grants_drop(&grants, name);
    if (r1 && r2) { refuse(name, MBJ_E_NOREC); return; }
    (void)count_pending();
    uart_print("[BUILD] "); uart_print(name); uart_print(" удалён вместе с записью и разрешением\n");
}

void build_cmd(const char* arg) {
    char verb[12];
    arg = skip(arg);
    if (!word(&arg, verb, sizeof verb)) { usage(); show_proposal(); return; }
    arg = skip(arg);
    if (same(verb, "probe") || same(verb, "table")) cmd_new(verb, arg);
    else if (same(verb, "list")) cmd_list();
    else if (same(verb, "allow")) cmd_allow(arg);
    else if (same(verb, "run")) cmd_run(arg);
    else if (same(verb, "drop")) cmd_drop(arg);
    else usage();
}

static void put_s(char* b, unsigned* n, const char* s) { while (*s && *n < 200) b[(*n)++] = *s++; b[*n] = 0; }
static void put_u(char* b, unsigned* n, uint64_t v) {
    char t[21]; unsigned k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k && *n < 200) b[(*n)++] = t[--k];
    b[*n] = 0;
}

void build_mind_tick(const mind_t* m, const mind_sample_t* s, uint32_t now_s) {
    static uint8_t synced;
    mbj_job_t job;
    mbj_rate_t r;
    uint64_t f = spin_lock_irqsave(&note_lock);
    r = rate;
    spin_unlock_irqrestore(&note_lock, f);
    if (!mind_build_propose(m, s, now_s, &r, &job)) return;
    if (!synced && kfs_mounted()) {                    /* число ожидающих решения после перезагрузки - с диска, один раз */
        synced = 1;
        r.pending = count_pending();
        if (!mind_build_propose(m, s, now_s, &r, &job)) return;
    }
    f = spin_lock_irqsave(&note_lock);
    mbj_rate_note_proposed(&rate, now_s);
    last_prop.have = 1; last_prop.job = job; last_prop.at_s = now_s;
    spin_unlock_irqrestore(&note_lock, f);
    char line[224]; unsigned n = 0;
    put_s(line, &n, "[BUILD] Разум предлагает задание: калибровочный зонд процессора, выполнить: build probe ");
    put_u(line, &n, job.p[0]); put_s(line, &n, " "); put_u(line, &n, job.p[1]);
    put_s(line, &n, " (сам Разум не собирает, не разрешает и не запускает; решение за человеком)\n");
    uart_print_serial(line);
}
