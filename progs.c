#include "progs.h"
#include "kfs.h"
#include "proc.h"
#include "immune.h"
#include "trust.h"
#include "life.h"
#include "fsrv.h"
#include "uart.h"
#include "cpu.h"
#include "prog_queue.h"
#include "manifest.h"      /* создаётся сборкой (kfstool manifest) */
#include "heap.h"
#include "pmm.h"
#include "mind_sys.h"

/* Размер программы ограничен только памятью: образ читается в буфер из кучи по размеру файла, но не больше
   четверти свободной ОЗУ (остальное нужно самой программе и системе). */
#define NAME_MAX (KFS_NAME_MAX + 1)

static prog_queue_t prog_requests = PROG_QUEUE_INIT;

static int name_ok(const char* n) {
    if (!n[0]) return 0;
    for (unsigned i = 0; n[i]; i++) {
        char c = n[i];
        if (i >= NAME_MAX - 1 || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

static uint64_t image_cap(void) { return (uint64_t)pmm_get_free_memory_kb() * 1024 / 4; }

static void hex(const uint8_t* d, unsigned n) {
    for (unsigned i = 0; i < n; i++) { uart_putchar("0123456789abcdef"[d[i] >> 4]); uart_putchar("0123456789abcdef"[d[i] & 15]); }
}

/* Допуск запуска: бюджет Разума (сколько экземпляров, сколько всего). 1 - можно. */
static int admit(const char* name) {
    static proc_stat_t ps[256];
    unsigned np = proc_stats(ps, 256), same = 0;
    for (unsigned i = 0; i < np; i++) {
        unsigned k = 0; while (k < 15 && ps[i].name[k] && ps[i].name[k] == name[k]) k++;
        if (!ps[i].dying && ps[i].name[k] == name[k]) same++;
    }
    char why[96];
    if (!mind_sys_admit(name, same, why, sizeof why)) {
        uart_print("[KIRA MIND] запуск "); uart_print(name); uart_print(" не допущен: "); uart_print(why); uart_print("\n");
        life_note("denied-launch", name, (int)same);
        return 0;
    }
    return 1;
}

static int prog_run_ex(const char* name, const char* grant, const char* args);
int prog_run(const char* name) { return prog_run_ex(name, 0, 0); }
int prog_run_args(const char* name, const char* args) { return prog_run_ex(name, 0, args && *args ? args : 0); }
static int prog_run_grant(const char* name, const char* grant) { return prog_run_ex(name, grant, 0); }

static int prog_run_ex(const char* name, const char* grant, const char* args) {
    if (!kfs_mounted()) { uart_print("[PROG] нет файловой системы\n"); return -1; }
    if (!name_ok(name)) { uart_print("[PROG] недопустимое имя программы\n"); return -1; }
    if (!admit(name)) return -4;
    char path[NAME_MAX + 8]; unsigned n = 0;
    for (const char* p = "/bin/"; *p; p++) path[n++] = *p;
    for (const char* p = name; *p; p++) path[n++] = *p;
    path[n] = 0;

    kfs_stat_t st;
    int r = kfs_stat(path, &st);
    if (r) { uart_print("[PROG] "); uart_print(path); uart_print(": "); uart_print(kfs_strerror(r)); uart_print(" (программы кладёт на диск сборка)\n"); return -1; }
    if (st.type != KFS_FILE || st.size == 0) { uart_print("[PROG] "); uart_print(path); uart_print(": неподходящий файл\n"); return -1; }
    if (st.size > image_cap()) { uart_print("[PROG] "); uart_print(path); uart_print(": программа больше, чем позволяет свободная память\n"); return -1; }
    uint8_t* image = (uint8_t*)kmalloc((size_t)st.size);
    if (!image) { uart_print("[PROG] нет памяти под образ программы\n"); return -1; }
    int64_t got = kfs_read(path, 0, image, (size_t)st.size);
    if (got != (int64_t)st.size) { kfree(image); uart_print("[PROG] ошибка чтения "); uart_print(path); uart_print("\n"); return -1; }

    uint8_t digest[32];
    sha256(image, (size_t)st.size, digest);
    trust_verdict_t t = trust_check(PROG_MANIFEST, PROG_MANIFEST_COUNT, name, digest);
    if (t != TRUST_OK) {
        immune_verdict_t v = immune_classify(image, (size_t)st.size);      /* заносит в карантин, запоминается в опыте */
        uart_print("[IMMUNE] "); uart_print(path);
        uart_print(t == TRUST_TAMPERED ? ": содержимое не совпадает с манифестом сборки (подмена?)" : ": нет в манифесте сборки (чужая программа)");
        uart_print(v == IMMUNE_HOSTILE ? ", сработало антитело" : "");
        uart_print(". Запуск отклонён, SHA-256 ");
        hex(digest, 8); uart_print("...\n");
        immune_phagocytose(image, (size_t)st.size);
        kfree(image);
        life_note("denied", name, (int)t);
        return -2;
    }
    immune_register_self(digest);
    (void)fsrv_sandbox(name);                                              /* песочница /data/<имя> создаётся только для доверенной программы */
    int pid = proc_spawn_elf_ex(name, image, (size_t)st.size, grant, args);
    immune_phagocytose(image, (size_t)st.size);                            /* копия в буфере больше не нужна */
    kfree(image);
    if (pid < 0) return -3;
    life_note("start", name, 0);
    return pid;
}

/* runlocal: запуск собранного пользователем ELF из /data/kcc/<файл> как программы local-<имя> БЕЗ прав (caps_for_program даёт 0 для
   всех имён с этой приставкой) и со своей песочницей /data/local-<имя>. Манифест сборки здесь не используется и не обходится:
   /bin по-прежнему запускается только по манифесту (prog_run_ex). Доверие даёт явная команда пользователя; она записывается в журнал.
   Перед запуском образ проходит immune_classify: враждебный (антитело) не запускается, неопознанный запускается (ровно потому, что
   пользователь его собрал и запустил сам). Результат: pid или -1 (нет файла/имя), -2 (враждебный), -3 (не принят как ELF), -4 (бюджет). */
int prog_run_local(const char* file) {
    if (!kfs_mounted()) { uart_print("[LOCAL] нет файловой системы\n"); return -1; }
    char name[16];
    if (!fsrv_local_name(file, name)) {
        uart_print("[LOCAL] недопустимое имя файла: до первой точки строчные латинские буквы, цифры, - и _, не больше 9 знаков\n");
        return -1;
    }
    char path[KFS_NAME_MAX + 16]; unsigned n = 0;
    for (const char* p = "/data/kcc/"; *p; p++) path[n++] = *p;
    for (const char* p = file; *p; p++) path[n++] = *p;
    path[n] = 0;
    kfs_stat_t st;
    int r = kfs_stat(path, &st);
    if (r) { uart_print("[LOCAL] "); uart_print(path); uart_print(": "); uart_print(kfs_strerror(r)); uart_print(" (собрать: cc исходник.c "); uart_print(file); uart_print(")\n"); return -1; }
    if (st.type != KFS_FILE || st.size == 0) { uart_print("[LOCAL] "); uart_print(path); uart_print(": неподходящий файл\n"); return -1; }
    if (st.size > image_cap()) { uart_print("[LOCAL] "); uart_print(path); uart_print(": больше, чем позволяет свободная память\n"); return -1; }
    if (!admit(name)) return -4;
    uint8_t* image = (uint8_t*)kmalloc((size_t)st.size);
    if (!image) { uart_print("[LOCAL] нет памяти под образ программы\n"); return -1; }
    if (kfs_read(path, 0, image, (size_t)st.size) != (int64_t)st.size) { kfree(image); uart_print("[LOCAL] ошибка чтения "); uart_print(path); uart_print("\n"); return -1; }
    immune_verdict_t v = immune_classify(image, (size_t)st.size);
    if (v == IMMUNE_HOSTILE) {
        uart_print("[IMMUNE] "); uart_print(path); uart_print(": сработало антитело, запуск отклонён\n");
        immune_phagocytose(image, (size_t)st.size);
        kfree(image);
        life_note("local-denied", name, (int)v);
        return -2;
    }
    uart_print("[LOCAL] "); uart_print(path); uart_print(" запускается по явной команде пользователя как '"); uart_print(name);
    uart_print("' без прав, иммунитет: "); uart_print(v == IMMUNE_SELF ? "своя" : "неопознанная (не из манифеста сборки)"); uart_print("\n");
    life_note("local-trust", name, (int)v);                                 /* доверие дано командой пользователя, не манифестом */
    (void)fsrv_sandbox(name);
    int pid = proc_spawn_elf_ex(name, image, (size_t)st.size, 0, 0);
    immune_phagocytose(image, (size_t)st.size);
    kfree(image);
    if (pid < 0) { uart_print("[LOCAL] файл не принят как программа (см. сообщение выше)\n"); return -3; }
    life_note("start", name, 0);
    return pid;
}

prog_request_status_t prog_request_file(const char* name, const char* file, uint32_t* request_id) {
    return prog_queue_submit(&prog_requests, name, file, request_id);
}
prog_request_status_t prog_request(const char* name, uint32_t* request_id) {
    return prog_request_file(name, 0, request_id);
}
prog_result_state_t prog_request_take_result(uint32_t request_id, int* result) {
    return prog_queue_take_result(&prog_requests, request_id, result);
}
void prog_poll(void) {
    char name[PROG_QUEUE_NAME_CAP], file[PROG_QUEUE_FILE_CAP];
    uint32_t request_id;
    if (!prog_queue_take(&prog_requests, name, file, &request_id)) return;
    int result = prog_run_grant(name, file[0] ? file : 0);
    (void)prog_queue_complete(&prog_requests, request_id, result);
}

/* Список /bin: имена собираются в буфер из кучи (сколько программ - столько записей), затем проверяется каждая. */
typedef struct { char (*names)[NAME_MAX]; unsigned n, cap; } list_t;
static int collect(void* c, const char* n, const kfs_stat_t* s) {
    list_t* l = (list_t*)c;
    if (s->type != KFS_FILE) return 0;
    if (l->n == l->cap) {
        unsigned nc = l->cap ? l->cap * 2 : 32;
        char (*nn)[NAME_MAX] = (char (*)[NAME_MAX])kmalloc((size_t)nc * NAME_MAX);
        if (!nn) return 1;
        for (unsigned i = 0; i < l->n; i++) for (unsigned k = 0; k < NAME_MAX; k++) nn[i][k] = l->names[i][k];
        if (l->names) kfree(l->names);
        l->names = nn; l->cap = nc;
    }
    unsigned i = 0;
    while (n[i] && i < NAME_MAX - 1) { l->names[l->n][i] = n[i]; i++; }
    l->names[l->n][i] = 0;
    l->n++;
    return 0;
}

void prog_list(void) {
    if (!kfs_mounted()) { uart_print("нет файловой системы\n"); return; }
    list_t l = { 0, 0, 0 };
    int r = kfs_list("/bin", collect, &l);                    /* обратный вызов не обращается к ФС: сначала собираем имена */
    if (r) { uart_print("/bin: "); uart_print(kfs_strerror(r)); uart_print(" (программы кладёт на диск сборка)\n"); if (l.names) kfree(l.names); return; }
    if (!l.n) { uart_print("/bin пуст\n"); if (l.names) kfree(l.names); return; }
    for (unsigned i = 0; i < l.n; i++) {
        char path[NAME_MAX + 8]; unsigned n = 0;
        for (const char* p = "/bin/"; *p; p++) path[n++] = *p;
        for (const char* p = l.names[i]; *p; p++) path[n++] = *p;
        path[n] = 0;
        kfs_stat_t st;
        const char* verdict = "?";
        uint8_t* image = 0;
        if (kfs_stat(path, &st) == 0 && st.size > 0 && st.size <= image_cap() && (image = (uint8_t*)kmalloc((size_t)st.size)) &&
            kfs_read(path, 0, image, (size_t)st.size) == (int64_t)st.size) {
            uint8_t d[32]; sha256(image, (size_t)st.size, d);
            trust_verdict_t t = trust_check(PROG_MANIFEST, PROG_MANIFEST_COUNT, l.names[i], d);
            verdict = t == TRUST_OK ? "доверенная" : t == TRUST_TAMPERED ? "ПОДМЕНЕНА" : "чужая (нет в манифесте)";
        }
        if (image) kfree(image);
        uart_print("  "); uart_print(l.names[i]); uart_print("  "); uart_print(verdict); uart_print("\n");
    }
    kfree(l.names);
}
