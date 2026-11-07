#include "caps.h"
#include "fsrv.h"
#include "vfs.h"
#include "kfs.h"
#include "kdrv_format.h"
#include "kdrv_sig.h"

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

int fsrv_local_name(const char* file, char out[16]) {
    if (!fsrv_name_ok(file)) return 0;
    unsigned n = 0;
    for (const char* pre = "local-"; *pre; pre++) out[n++] = *pre;
    unsigned k = 0;
    for (; file[k] && file[k] != '.'; k++) {
        char c = file[k];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') || n >= 15) return 0;
        out[n++] = c;
    }
    out[n] = 0;
    return k > 0 && fsrv_prog_ok(out);
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

extern void sha256(const void* data, size_t len, uint8_t out[32]);
#ifdef KIRA_HOST_TEST
#include <stdlib.h>
#define kdrv_tmp_alloc(n) malloc(n)
#define kdrv_tmp_free(p) free(p)
#define kdrv_installed(stem) ((void)(stem))
#define kdrv_ctl_post(op, stem) ((void)(op), (void)(stem), 0)
#else
#include "heap.h"
#include "kdrv.h"
#define kdrv_tmp_alloc(n) kmalloc(n)
#define kdrv_tmp_free(p) kfree(p)
#define kdrv_installed(stem) kdrv_request_rescan()          /* установленный пакет сразу получает свою очередь на устройство */
#define kdrv_ctl_post(op, stem) kdrv_ctl_request((op) == FS_OP_DRV_START ? KDRV_CTL_START : KDRV_CTL_STOP, (stem))
#endif

int fsrv_drv_name(const char* file, char stem[16], int* kind) {
    size_t n = slen(file), ext;
    if (!kind) return 0;
    if (n > 5 && file[n - 5] == '.' && file[n - 4] == 'k' && file[n - 3] == 'd' && file[n - 2] == 'r' && file[n - 1] == 'v') { ext = 5; *kind = 0; }
    else if (n > 4 && file[n - 4] == '.' && file[n - 3] == 'r' && file[n - 2] == 'e' && file[n - 1] == 'c') { ext = 4; *kind = 1; }
    else if (n > 4 && file[n - 4] == '.' && file[n - 3] == 's' && file[n - 2] == 'i' && file[n - 1] == 'g') { ext = 4; *kind = 2; }
    else return 0;
    size_t k = n - ext;
    if (k < 1 || k > KDRV_NAME_MAX) return 0;
    for (size_t i = 0; i < k; i++) {
        char c = file[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
        stem[i] = c;
    }
    stem[k] = 0;
    return 1;
}

static int drv_dir(void) {
    kfs_stat_t st;
    if (kfs_stat("/drv", &st) == 0) return st.type == KFS_DIR ? 0 : -1;
    return kfs_mkdir("/drv") == 0 ? 0 : -1;
}

static int streq16(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

/* Программы, которым разрешены FS_OP_DRV_* и сборка драйвера kcc: drvget (консоль) и drvman (окно «Драйверы», тот же код drvget). */
static int drv_tool(const char* who) { return who && (streq16(who, "drvget") || streq16(who, "drvman")); }

/* FS_OP_DRV_*: пакеты драйверов. Вызывающий - только drvget или drvman (проверено выше и здесь по имени процесса). */
static void drv_execute(fsrv_req_t* r) {
    if (!drv_tool(r->prog)) { r->result = FS_E_DENIED; return; }
    char path[48];
    if (r->op == FS_OP_DRV_LIST) {
        scan_t s = { 0, r->data, (size_t)r->len, 0, 0, 0, 0 };
        int e = kfs_list("/drv", scan_cb, &s);
        r->result = e ? e : (int64_t)s.used;
        return;
    }
    char stem[16]; int kind;
    if (!fsrv_drv_name(r->name, stem, &kind)) { r->result = FS_E_DENIED; return; }
    { size_t n = 0; for (const char* a = "/drv/"; *a; a++) path[n++] = *a; for (size_t i = 0; r->name[i]; i++) path[n++] = r->name[i]; path[n] = 0; }
    kfs_stat_t st;
    switch (r->op) {
    case FS_OP_DRV_START: case FS_OP_DRV_STOP: {
        if (kind != 0) { r->result = FS_E_ARG; return; }
        if (r->op == FS_OP_DRV_START) {                                   /* запускать можно только установленный пакет */
            int e = kfs_stat(path, &st);
            if (e || st.type != KFS_FILE) { r->result = e ? e : KFS_ERR_ISDIR; return; }
        }
        r->result = kdrv_ctl_post(r->op, stem) == 0 ? 0 : FS_E_BUSY;
        return;
    }
    case FS_OP_DRV_STAT: {
        int e = kfs_stat(path, &st);
        r->result = e ? e : (st.type == KFS_FILE ? (int64_t)st.size : KFS_ERR_ISDIR);
        return;
    }
    case FS_OP_DRV_READ:
        if (r->len == 0 || r->len > fsrv_max_data()) { r->result = FS_E_ARG; return; }
        r->result = kfs_read(path, r->off, r->data, (size_t)r->len);
        return;
    case FS_OP_DRV_REMOVE: {
        int e = kfs_stat(path, &st);
        if (e) { r->result = e; return; }
        if (st.type != KFS_FILE) { r->result = KFS_ERR_ISDIR; return; }
        if (kind == 2) {
            /* Сначала убрать подтверждение. Если это не удалось, подпись остаётся на месте:
               удаление .sig при сохранённом .rec не должно превращать пакет в unsigned/local. */
            char rp[48]; size_t n = 0;
            for (const char* a = "/drv/"; *a; a++) rp[n++] = *a;
            for (size_t i = 0; stem[i]; i++) rp[n++] = stem[i];
            for (const char* a = ".rec"; *a; a++) rp[n++] = *a;
            rp[n] = 0;
            int re = kfs_stat(rp, &st);
            if (re == 0) {
                if (st.type != KFS_FILE) { r->result = KFS_ERR_ISDIR; return; }
                re = kfs_unlink(rp);
                if (re) { r->result = re; return; }
            } else if (re != KFS_ERR_NOENT) { r->result = re; return; }
            r->result = kfs_unlink(path);
            return;
        }
        r->result = kfs_unlink(path);
        if (r->result != 0 || kind != 0) return;
        const char* suffix[2] = { ".rec", ".sig" };
        for (unsigned q = 0; q < 2; q++) {
            char companion[48]; size_t n = 0;
            for (const char* a = "/drv/"; *a; a++) companion[n++] = *a;
            for (size_t i = 0; stem[i]; i++) companion[n++] = stem[i];
            for (const char* a = suffix[q]; *a; a++) companion[n++] = *a;
            companion[n] = 0;
            if (kfs_stat(companion, &st) == 0 && st.type == KFS_FILE) {
                int ce = kfs_unlink(companion);
                if (ce && r->result == 0) r->result = ce;
            }
        }
        return;
    }
    case FS_OP_DRV_WRITE: {
        uint64_t max_len = kind == 0 ? sizeof(kdrv_header_t) + KDRV_ELF_MAX : kind == 1 ? sizeof(kdrv_rec_t) : sizeof(kdrv_sig_t);
        if (r->off != 0 || r->len == 0 || r->len > max_len || r->len > fsrv_max_data() ||
            (kind == 1 && r->len != sizeof(kdrv_rec_t)) || (kind == 2 && r->len != sizeof(kdrv_sig_t))) { r->result = FS_E_ARG; return; }
        if (drv_dir() != 0) { r->result = KFS_ERR_IO; return; }
        if (kind == 0) {
            kdrv_header_t h;
            int rc = kdrv_header_check(r->data, (uint32_t)r->len, &h);
            if (rc != KDRV_OK || !streq16(h.name, stem)) { r->result = FS_E_ARG; return; }
        } else if (kind == 1) {
            kdrv_rec_t rec;
            if (kdrv_rec_check(r->data, (uint32_t)r->len, &rec) != KDRV_OK) { r->result = FS_E_ARG; return; }
            char kp[48]; size_t n = 0;
            for (const char* a = "/drv/"; *a; a++) kp[n++] = *a;
            for (size_t i = 0; stem[i]; i++) kp[n++] = stem[i];
            for (const char* a = ".kdrv"; *a; a++) kp[n++] = *a;
            kp[n] = 0;
            if (kfs_stat(kp, &st) != 0 || st.type != KFS_FILE || st.size > sizeof(kdrv_header_t) + KDRV_ELF_MAX) { r->result = FS_E_DENIED; return; }
            uint8_t* b = (uint8_t*)kdrv_tmp_alloc((size_t)st.size);
            if (!b) { r->result = KFS_ERR_NOSPC; return; }
            kdrv_header_t h;
            int ok = kfs_read(kp, 0, b, (size_t)st.size) == (int64_t)st.size && kdrv_header_check(b, (uint32_t)st.size, &h) == KDRV_OK && streq16(h.name, stem);
            uint8_t d1[32], d2[32];
            if (ok) { sha256(b + sizeof h, h.elf_size, d1); sha256(b, (size_t)st.size, d2); }
            kdrv_tmp_free(b);
            if (!ok) { r->result = FS_E_DENIED; return; }
            for (int i = 0; i < 32; i++) if (d1[i] != rec.elf_sha[i] || d2[i] != rec.pkg_sha[i]) { r->result = FS_E_DENIED; return; }
            if (rec.caps_ok & ~h.caps) { r->result = FS_E_DENIED; return; }
        } else {
            kdrv_sig_t sig;
            const uint8_t* sig_src = (const uint8_t*)r->data;
            uint8_t* sig_dst = (uint8_t*)&sig;
            for (size_t i = 0; i < sizeof sig; i++) sig_dst[i] = sig_src[i];
            uint8_t key_nonzero = 0;
            for (unsigned i = 0; i < sizeof sig.key_id; i++) key_nonzero |= sig.key_id[i];
            if (sig.magic != KDRV_SIG_MAGIC || sig.format != KDRV_SIG_FORMAT || sig.size != sizeof sig ||
                !key_nonzero || !sig.package_size || !sig.not_before || sig.not_after <= sig.not_before ||
                sig.not_after > 0x7FFFFFFFFFFFFFFFULL) { r->result = FS_E_ARG; return; }
            char kp[48]; size_t n = 0;
            for (const char* a = "/drv/"; *a; a++) kp[n++] = *a;
            for (size_t i = 0; stem[i]; i++) kp[n++] = stem[i];
            for (const char* a = ".kdrv"; *a; a++) kp[n++] = *a;
            kp[n] = 0;
            if (kfs_stat(kp, &st) != 0 || st.type != KFS_FILE || st.size != sig.package_size ||
                st.size > sizeof(kdrv_header_t) + KDRV_ELF_MAX) { r->result = FS_E_DENIED; return; }
            uint8_t* b = (uint8_t*)kdrv_tmp_alloc((size_t)st.size);
            if (!b) { r->result = KFS_ERR_NOSPC; return; }
            uint8_t digest[32]; int ok = kfs_read(kp, 0, b, (size_t)st.size) == (int64_t)st.size;
            if (ok) sha256(b, (size_t)st.size, digest);
            kdrv_tmp_free(b);
            int bad = 0;
            if (ok) for (int i = 0; i < 32; i++) bad |= digest[i] ^ sig.package_sha256[i];
            if (!ok || bad) { r->result = FS_E_DENIED; return; }
            char rp[48]; n = 0;
            for (const char* a = "/drv/"; *a; a++) rp[n++] = *a;
            for (size_t i = 0; stem[i]; i++) rp[n++] = stem[i];
            for (const char* a = ".rec"; *a; a++) rp[n++] = *a;
            rp[n] = 0;
            if (kfs_stat(rp, &st) != 0 || st.type != KFS_FILE || st.size != sizeof(kdrv_rec_t)) { r->result = FS_E_DENIED; return; }
        }
        kfs_info_t fi; kfs_info(&fi);
        uint64_t keep = (uint64_t)fi.total_blocks / 16, need = (r->len + 4095) / 4096 + 1;
        int exists = kfs_stat(path, &st) == 0;
        uint64_t had = exists ? (st.size + 4095) / 4096 : 0;
        if (need > had && (uint64_t)fi.free_blocks < keep + (need - had)) { r->result = FS_E_QUOTA; return; }
        r->result = kfs_write(path, r->data, (size_t)r->len);
        if (r->result == 0 && kind == 0) {
            const char* suffix[2] = { ".rec", ".sig" };
            for (unsigned q = 0; q < 2; q++) {
                char companion[48]; size_t n = 0;
                for (const char* a = "/drv/"; *a; a++) companion[n++] = *a;
                for (size_t i = 0; stem[i]; i++) companion[n++] = stem[i];
                for (const char* a = suffix[q]; *a; a++) companion[n++] = *a;
                companion[n] = 0;
                int ce = kfs_unlink(companion);
                if (ce != 0 && ce != KFS_ERR_NOENT) { r->result = ce; return; }
            }
        }
        if (r->result == 0 && kind != 0) kdrv_installed(stem);
        return;
    }
    default: r->result = FS_E_ARG; return;
    }
}

void fsrv_execute(fsrv_req_t* r) {
    r->result = FS_E_ARG;
    if (!kfs_mounted()) { r->result = KFS_ERR_NOFS; return; }
    if (!fsrv_prog_ok(r->prog)) { r->result = FS_E_DENIED; return; }
    if (r->op >= FS_OP_DRV_STAT && r->op <= FS_OP_DRV_LAST) {
        if (r->op != FS_OP_DRV_LIST && !fsrv_name_ok(r->name)) { r->result = FS_E_DENIED; return; }
        drv_execute(r);
        return;
    }
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
static int64_t sys_op(const fsrv_user_req_t* u, const char* pre) {
    char name[FSRV_NAME_MAX + 1], path[24 + FSRV_NAME_MAX];
    if (u->name_len == 0 || u->name_len > FSRV_NAME_MAX || copy_from_user(name, u->name_ptr, u->name_len) != 0) return FS_E_ARG;
    name[u->name_len] = 0;
    if (!fsrv_name_ok(name)) return FS_E_DENIED;
    unsigned n = 0;
    while (pre[n]) { path[n] = pre[n]; n++; }
    for (unsigned i = 0; name[i]; i++) path[n++] = name[i];
    path[n] = 0;
    if (u->op == FS_OP_SYS_STAT || u->op == FS_OP_INC_STAT) {
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
    uint32_t caps = proc_current_caps();
    if (u.op == FS_OP_SYS_READ || u.op == FS_OP_SYS_STAT) { if (!(caps & CAP_SYSFILES)) return proc_cap_denied(CAP_SYSFILES, 10), FS_E_DENIED; return sys_op(&u, "/etc/ssl/"); }
    if (u.op == FS_OP_INC_READ || u.op == FS_OP_INC_STAT) {       /* заголовки для компилятора: только kcc (имя известно ядру по манифесту) и только при CAP_FS */
        const char* who = proc_current_name();
        int is_kcc = who && who[0] == 'k' && who[1] == 'c' && who[2] == 'c' && !who[3];
        int is_drvget = drv_tool(who);   /* drvget и drvman собирают драйвер тем же компилятором, встроенным в них */
        if (!(caps & CAP_FS) || !(is_kcc || is_drvget)) return proc_cap_denied(CAP_FS, 10), FS_E_DENIED;
        return sys_op(&u, "/etc/kcc/include/");
    }
    if (u.op >= FS_OP_DRV_STAT && u.op <= FS_OP_DRV_LAST) {      /* пакеты драйверов в /drv: только drvget и drvman (имя известно ядру по манифесту) и только при CAP_FS */
        if (!(caps & CAP_FS) || !drv_tool(proc_current_name())) return proc_cap_denied(CAP_FS, 10), FS_E_DENIED;
    }
    else if (u.op >= FS_OP_GRANT_READ && u.op <= FS_OP_GRANT_NAME) { if (!(caps & CAP_GRANT)) return proc_cap_denied(CAP_GRANT, 10), FS_E_DENIED; return grant_op(&u); }
    if ((u.op < FS_OP_READ || u.op > FS_OP_REMOVE) && !(u.op >= FS_OP_DRV_STAT && u.op <= FS_OP_DRV_LAST)) return FS_E_ARG;
    if (!(caps & CAP_FS)) return proc_cap_denied(CAP_FS, 10), FS_E_DENIED;
    const char* prog = proc_current_name();
    if (!fsrv_prog_ok(prog)) return FS_E_DENIED;

    char name[FSRV_NAME_MAX + 1]; name[0] = 0;
    if (u.op != FS_OP_LIST && u.op != FS_OP_DRV_LIST) {
        if (u.name_len == 0 || u.name_len > FSRV_NAME_MAX || copy_from_user(name, u.name_ptr, u.name_len) != 0) return FS_E_ARG;
        name[u.name_len] = 0;
        if (!fsrv_name_ok(name)) { life_note("deny-fs", prog, (int)u.op); return FS_E_DENIED; }
    }
    int to_user = (u.op == FS_OP_READ || u.op == FS_OP_LIST || u.op == FS_OP_DRV_READ || u.op == FS_OP_DRV_LIST), from_user = (u.op == FS_OP_WRITE || u.op == FS_OP_DRV_WRITE);
    if (to_user || from_user) {
        if (u.buf_len > fsrv_max_data() || (u.buf_len == 0 && u.op != FS_OP_WRITE && u.op != FS_OP_DRV_WRITE) || (u.buf_len && !user_range(u.buf_ptr, u.buf_len, to_user))) return FS_E_ARG;
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
