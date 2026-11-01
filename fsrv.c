#include "fsrv.h"
#include "vfs.h"
#include "kfs.h"

static size_t slen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }

int fsrv_prog_ok(const char* p) {
    if (!p[0]) return 0;
    for (unsigned i = 0; p[i]; i++) {
        char c = p[i];
        if (i >= 15 || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

int fsrv_name_ok(const char* n) {
    size_t len = slen(n);
    if (len == 0 || len > FSRV_NAME_MAX) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        int alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (i == 0 ? !alnum : !(alnum || c == '.' || c == '-' || c == '_')) return 0;
    }
    return 1;
}

static size_t mkpath(char* out, const char* prog, const char* name) {
    size_t n = 0;
    const char* a = "/data/";
    while (*a) out[n++] = *a++;
    while (*prog) out[n++] = *prog++;
    if (name) { out[n++] = '/'; while (*name) out[n++] = *name++; }
    out[n] = 0;
    return n;
}

int fsrv_sandbox(const char* prog) {
    if (!kfs_mounted() || !fsrv_prog_ok(prog)) return -1;
    char path[40]; kfs_stat_t st;
    if (kfs_stat("/data", &st) != 0 && kfs_mkdir("/data") != 0) return -1;
    mkpath(path, prog, 0);
    if (kfs_stat(path, &st) == 0) return st.type == KFS_DIR ? 0 : -1;
    return kfs_mkdir(path) == 0 ? 0 : -1;
}

typedef struct { unsigned files; uint8_t* out; size_t cap, used; int full; const char* want; int found; } scan_t;
static int scan_cb(void* c, const char* name, const kfs_stat_t* st) {
    scan_t* s = (scan_t*)c;
    if (st->type != KFS_FILE) return 0;
    s->files++;
    if (s->want) { const char* a = name; const char* b = s->want; while (*a && *a == *b) { a++; b++; } if (*a == *b) s->found = 1; }
    if (s->out) {
        size_t n = slen(name);
        if (s->used + n + 1 > s->cap) { s->full = 1; return 0; }
        for (size_t i = 0; i < n; i++) s->out[s->used++] = (uint8_t)name[i];
        s->out[s->used++] = '\n';
    }
    return 0;
}

uint64_t fsrv_max_files(void) {
    kfs_info_t fi; kfs_info(&fi);
    uint64_t n = fi.inodes / 8;
    return n < 32 ? 32 : n;
}

#ifdef KIRA_HOST_TEST
uint64_t fsrv_max_data(void) { return 8u << 20; }
#else
#include "pmm.h"
uint64_t fsrv_max_data(void) {
    uint64_t b = (uint64_t)pmm_get_free_memory_kb() * 1024 / 4;
    return b < 65536 ? 65536 : b;
}
#endif

void fsrv_execute(fsrv_req_t* r) {
    r->result = FS_E_ARG;
    if (!kfs_mounted()) { r->result = KFS_ERR_NOFS; return; }
    if (!fsrv_prog_ok(r->prog)) { r->result = FS_E_DENIED; return; }
    char path[96];
    if (r->op == FS_OP_LIST) {
        if (fsrv_sandbox(r->prog) != 0) { r->result = KFS_ERR_IO; return; }
        mkpath(path, r->prog, 0);
        scan_t s = { 0, r->data, (size_t)r->len, 0, 0, 0, 0 };
        int e = kfs_list(path, scan_cb, &s);
        r->result = e ? e : (int64_t)s.used;
        return;
    }
    if (!fsrv_name_ok(r->name)) { r->result = FS_E_DENIED; return; }
    mkpath(path, r->prog, r->name);
    switch (r->op) {
    case FS_OP_READ: {
        if (r->len == 0 || r->len > fsrv_max_data()) { r->result = FS_E_ARG; return; }
        r->result = kfs_read(path, r->off, r->data, (size_t)r->len);
        return;
    }
    case FS_OP_WRITE: {
        if (r->len > fsrv_max_data()) { r->result = FS_E_ARG; return; }
        if (fsrv_sandbox(r->prog) != 0) { r->result = KFS_ERR_IO; return; }
        kfs_stat_t st;
        int exists = kfs_stat(path, &st) == 0;
        kfs_info_t fi; kfs_info(&fi);
        uint64_t keep = (uint64_t)fi.total_blocks / 16, need = (r->len + 4095) / 4096 + 1;
        uint64_t had = exists ? (st.size + 4095) / 4096 : 0;
        if (need > had && (uint64_t)fi.free_blocks < keep + (need - had)) { r->result = FS_E_QUOTA; return; }   /* системе - 1/16 тома */
        if (!exists) {                                                   /* новый файл: проверка квоты */
            char dir[40]; mkpath(dir, r->prog, 0);
            scan_t s = { 0, 0, 0, 0, 0, 0, 0 };
            int e = kfs_list(dir, scan_cb, &s);
            if (e) { r->result = e; return; }
            if (s.files >= fsrv_max_files()) { r->result = FS_E_QUOTA; return; }
        }
        r->result = kfs_write(path, r->data, (size_t)r->len);
        return;
    }
    case FS_OP_STAT: {
        kfs_stat_t st;
        int e = kfs_stat(path, &st);
        r->result = e ? e : (st.type == KFS_FILE ? (int64_t)st.size : KFS_ERR_ISDIR);
        return;
    }
    case FS_OP_REMOVE: {
        kfs_stat_t st;
        int e = kfs_stat(path, &st);
        if (e) { r->result = e; return; }
        r->result = st.type == KFS_FILE ? kfs_unlink(path) : KFS_ERR_ISDIR;       /* каталоги программа не трогает */
        return;
    }
    default: r->result = FS_E_ARG; return;
    }
}

#ifndef KIRA_HOST_TEST
#include "vmm.h"
#include "proc.h"
#include "sched.h"
#include "cpu.h"
#include "spinlock.h"
#include "life.h"
#include "timer.h"

#include "heap.h"
#include "kmutex.h"

/* Запрос выполняется прямо в потоке вызвавшей программы (на любом ядре): KiraFS и блочное устройство под своими
   мьютексами. Мьютекс сервиса делает атомарными многошаговые операции (проверка квоты и запись, stat и удаление)
   между программами. Раньше запрос шёл через один общий слот, который основной поток ядра опрашивал раз в 10 мс. */
static kmutex_t fsrv_mx = KMUTEX_INIT;

#include "uaccess.h"
/* Файл, выданный программе при запуске (Проводник открыл его в программе просмотра): только чтение, только он.
   Путь берётся из процесса (ядро записало его до первого потока), а не из запроса - другой файл так не прочесть. */
static int64_t grant_op(const fsrv_user_req_t* u) {
    const char* path = proc_current_grant();
    if (!path[0]) return FS_E_DENIED;
    if (u->op == FS_OP_GRANT_STAT) {
        kfs_stat_t st;
        int e = vfs_stat(path, &st);
        return e ? e : (st.type == KFS_FILE ? (int64_t)st.size : FS_E_DENIED);
    }
    if (!u->buf_len || u->buf_len > fsrv_max_data() || !user_range(u->buf_ptr, u->buf_len, 1)) return FS_E_ARG;
    if (u->op == FS_OP_GRANT_NAME) {
        uint64_t n = 0; while (path[n]) n++;
        if (n + 1 > u->buf_len) return FS_E_ARG;
        return copy_to_user(u->buf_ptr, path, n + 1) ? FS_E_ARG : (int64_t)n;
    }
    uint8_t* b = (uint8_t*)kmalloc(u->buf_len);
    if (!b) return KFS_ERR_NOSPC;
    int64_t r = vfs_read(path, u->off, b, (size_t)u->buf_len);
    if (r > 0 && copy_to_user(u->buf_ptr, b, (uint64_t)r) != 0) r = FS_E_ARG;
    kfree(b);
    return r;
}

/* Общие файлы системы для всех программ - только чтение и только из /etc/ssl (хранилище корневых сертификатов).
   Имя - одно имя файла по правилам песочницы (без «/» и «..»), путь собирает ядро. */
static int64_t sys_op(const fsrv_user_req_t* u) {
    char name[FSRV_NAME_MAX + 1], path[16 + FSRV_NAME_MAX];
    if (u->name_len == 0 || u->name_len > FSRV_NAME_MAX || copy_from_user(name, u->name_ptr, u->name_len) != 0) return FS_E_ARG;
    name[u->name_len] = 0;
    if (!fsrv_name_ok(name)) return FS_E_DENIED;
    const char* pre = "/etc/ssl/"; unsigned n = 0;
    while (pre[n]) { path[n] = pre[n]; n++; }
    for (unsigned i = 0; name[i]; i++) path[n++] = name[i];
    path[n] = 0;
    if (u->op == FS_OP_SYS_STAT) {
        kfs_stat_t st;
        int e = vfs_stat(path, &st);
        return e ? e : (st.type == KFS_FILE ? (int64_t)st.size : FS_E_DENIED);
    }
    if (!u->buf_len || u->buf_len > fsrv_max_data() || !user_range(u->buf_ptr, u->buf_len, 1)) return FS_E_ARG;
    uint8_t* b = (uint8_t*)kmalloc(u->buf_len);
    if (!b) return KFS_ERR_NOSPC;
    int64_t r = vfs_read(path, u->off, b, (size_t)u->buf_len);
    if (r > 0 && copy_to_user(u->buf_ptr, b, (uint64_t)r) != 0) r = FS_E_ARG;
    kfree(b);
    return r;
}

int64_t fsrv_syscall(uint64_t up) {
    fsrv_user_req_t u;                                                           /* своя копия: программа не изменит её во время проверки */
    if (copy_from_user(&u, up, sizeof u) != 0) return FS_E_ARG;
    if (u.op == FS_OP_SYS_READ || u.op == FS_OP_SYS_STAT) return sys_op(&u);
    if (u.op >= FS_OP_GRANT_READ && u.op <= FS_OP_GRANT_NAME) return grant_op(&u);
    if (u.op < FS_OP_READ || u.op > FS_OP_REMOVE) return FS_E_ARG;
    const char* prog = proc_current_name();
    if (!fsrv_prog_ok(prog)) return FS_E_DENIED;

    char name[FSRV_NAME_MAX + 1]; name[0] = 0;
    if (u.op != FS_OP_LIST) {
        if (u.name_len == 0 || u.name_len > FSRV_NAME_MAX || copy_from_user(name, u.name_ptr, u.name_len) != 0) return FS_E_ARG;
        name[u.name_len] = 0;
        if (!fsrv_name_ok(name)) { life_note("deny-fs", prog, (int)u.op); return FS_E_DENIED; }
    }
    int to_user = (u.op == FS_OP_READ || u.op == FS_OP_LIST), from_user = (u.op == FS_OP_WRITE);
    if (to_user || from_user) {
        if (u.buf_len > fsrv_max_data() || (u.buf_len == 0 && u.op != FS_OP_WRITE) || (u.buf_len && !user_range(u.buf_ptr, u.buf_len, to_user))) return FS_E_ARG;
    }

    fsrv_req_t* r = (fsrv_req_t*)kmalloc(sizeof(fsrv_req_t));
    if (!r) return FS_E_ARG;
    r->data = (uint8_t*)kmalloc(u.buf_len ? u.buf_len : 1);              /* по размеру запроса */
    if (!r->data) { kfree(r); return KFS_ERR_NOSPC; }
    r->op = u.op;
    for (unsigned i = 0; i < sizeof r->prog; i++) r->prog[i] = 0;
    for (unsigned i = 0; prog[i] && i < sizeof r->prog - 1; i++) r->prog[i] = prog[i];
    for (unsigned i = 0; i <= FSRV_NAME_MAX; i++) r->name[i] = name[i];
    r->off = u.off; r->len = u.buf_len;
    if (from_user && copy_from_user(r->data, u.buf_ptr, u.buf_len) != 0) { kfree(r->data); kfree(r); return FS_E_ARG; }   /* копия: программа не изменит данные во время записи */
    kmutex_lock(&fsrv_mx);
    fsrv_execute(r);
    kmutex_unlock(&fsrv_mx);
    int64_t res = r->result;
    if (to_user && res > 0) {
        if ((uint64_t)res > u.buf_len) res = (int64_t)u.buf_len;
        if (copy_to_user(u.buf_ptr, r->data, (uint64_t)res) != 0) res = FS_E_ARG;
    }
    kfree(r->data);
    kfree(r);
    return res;
}
#endif
