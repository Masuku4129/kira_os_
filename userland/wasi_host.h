/* WASI 0.1 (wasi_snapshot_preview1) для интерпретатора wasm.c: полный список функций preview1 с проверкой сигнатур, реальные файлы,
   стандартный ввод и аргументы. Включается из wasm.c (использует его mem, trap, alloc и вызовы Киры k_*).

   Файлы: файловая служба Киры плоская - /data/<программа>/<имя> (fsrv.c), без подкаталогов; запись заменяет файл целиком.
   Поэтому модулю доступен один преоткрытый каталог "/" (fd 3) - его песочница /data/wasm; имя файла [A-Za-z0-9._-], до 40 знаков.
   Открытый на чтение файл читается напрямую со смещением. Открытый на запись держится в буфере памяти (до WF_CAP байт, не больше
   WF_POOL таких файлов одновременно) и записывается службе при fd_close, fd_sync и fd_datasync.
   Стандартный ввод (fd 0): строки консоли через системный вызов STDIN (право CAP_STDIN). Аргументы: строка запуска, слова через пробел.
   Время: монотонные часы - наносекунды ядра; настенные - секунды UTC из часов реального времени (точность 1 с).
   Не поддержано и отвечает ENOSYS/ENOTSUP (никогда не ложным успехом): каталоги внутри песочницы, ссылки, сокеты, права и метки времени,
   fd_advise/fd_allocate/fd_renumber, разрешение часов, сигналы. */

#if WASI_PART == 1
#define WASI_FUNCS(X) \
    X(args_get,"ii") X(args_sizes_get,"ii") X(clock_res_get,"ii") X(clock_time_get,"ili") X(environ_get,"ii") X(environ_sizes_get,"ii") \
    X(fd_advise,"illi") X(fd_allocate,"ill") X(fd_close,"i") X(fd_datasync,"i") X(fd_fdstat_get,"ii") X(fd_fdstat_set_flags,"ii") \
    X(fd_fdstat_set_rights,"ill") X(fd_filestat_get,"ii") X(fd_filestat_set_size,"il") X(fd_filestat_set_times,"illi") X(fd_pread,"iiili") \
    X(fd_prestat_get,"ii") X(fd_prestat_dir_name,"iii") X(fd_pwrite,"iiili") X(fd_read,"iiii") X(fd_readdir,"iiili") X(fd_renumber,"ii") \
    X(fd_seek,"ilii") X(fd_sync,"i") X(fd_tell,"ii") X(fd_write,"iiii") X(path_create_directory,"iii") X(path_filestat_get,"iiiii") \
    X(path_filestat_set_times,"iiiilli") X(path_link,"iiiiiii") X(path_open,"iiiiillii") X(path_readlink,"iiiiii") \
    X(path_remove_directory,"iii") X(path_rename,"iiiiii") X(path_symlink,"iiiii") X(path_unlink_file,"iii") X(poll_oneoff,"iiii") \
    X(proc_exit,"i") X(proc_raise,"i") X(random_get,"ii") X(sched_yield,"") X(sock_accept,"iii") X(sock_recv,"iiiiii") X(sock_send,"iiiii") \
    X(sock_shutdown,"ii")

#define WASI_ENUM(n, s) IW_##n,
enum { WASI_FUNCS(WASI_ENUM) IW_COUNT };
#define WASI_NAME(n, s) #n,
static const char* const wasi_names[IW_COUNT] = { WASI_FUNCS(WASI_NAME) };
#define WASI_SIG(n, s) s,
static const char* const wasi_sigs[IW_COUNT] = { WASI_FUNCS(WASI_SIG) };

/* Коды ошибок WASI (errno preview1). */
enum { W_OK = 0, W_AGAIN = 6, W_BADF = 8, W_EXIST = 20, W_FAULT = 21, W_FBIG = 22, W_INTR = 27, W_INVAL = 28, W_IO = 29, W_ISDIR = 31,
       W_MFILE = 33, W_NAMETOOLONG = 37, W_NOENT = 44, W_NOSPC = 51, W_NOSYS = 52, W_NOTDIR = 54, W_NOTSUP = 58, W_SPIPE = 70, W_NOTCAPABLE = 76 };
enum { WT_CHR = 2, WT_DIR = 3, WT_REG = 4 };

static int wasi_sig_ok(u32 idx, const ftype_t* t) {
    static const u8 i32t = 0x7F, i64t = 0x7E;
    const char* s = wasi_sigs[idx]; u32 n = 0;
    while (s[n]) n++;
    if (t->np != n) return 0;
    for (u32 i = 0; i < n; i++) if (t->p[i] != (s[i] == 'l' ? i64t : i32t)) return 0;
    return idx == IW_proc_exit || idx == IW_proc_raise ? (idx == IW_proc_exit ? t->nr == 0 : t->nr == 1) : t->nr == 1;
}

#else
/* ---------- состояние ---------- */
#define WF_N 16                 /* открытых файлов и каталогов (fd 4..19); fd 3 - преоткрытый "/" */
#define WF_CAP (512u << 10)     /* предел размера файла, открытого на запись */
#define WF_POOL 4               /* файлов, открытых на запись одновременно */
typedef struct { u8 used, dir, rd, wr, append, dirty, pool; char name[44]; u64 pos, size; } wfile_t;
static wfile_t wf[WF_N];
static u8* wpool[WF_POOL]; static u8 wpool_used[WF_POOL];
static char wa_buf[512]; static u32 wa_off[17], wa_n;           /* argv: слова строки запуска */
static u32 wa_bytes;
static char* wlist; static u32 wlist_cap = 16384;

static void wasi_args_init(const char* argv0) {
    u32 o = 0; wa_n = 0;
    for (const char* s = argv0; *s && o + 1 < sizeof wa_buf; s++) wa_buf[o++] = *s;
    wa_buf[o++] = 0; wa_off[wa_n++] = 0;
    char raw[256]; i64 r = k_args(raw, sizeof raw);
    if (r > 0) {
        for (i64 i = 0; i < r && wa_n < 16; ) {
            while (i < r && raw[i] == ' ') i++;
            if (i >= r) break;
            if (o + 2 >= sizeof wa_buf) break;
            wa_off[wa_n++] = o;
            while (i < r && raw[i] != ' ' && o + 1 < sizeof wa_buf) wa_buf[o++] = raw[i++];
            wa_buf[o++] = 0;
        }
    }
    wa_bytes = o;
}

static int wput32(u32 p, u32 v, u64 lim) { if ((u64)p + 4 > lim) return 0; st32(mem + p, v); return 1; }
static int wput64(u32 p, u64 v, u64 lim) { if ((u64)p + 8 > lim) return 0; st64(mem + p, v); return 1; }

/* Имя файла из пути модуля относительно "/": "" и "." - сам каталог. 0 - имя в out, 1 - каталог, иначе errno. */
static int wasi_name(u32 p, u32 len, u64 lim, char out[44]) {
    if ((u64)p + len > lim) return -W_FAULT;
    const u8* s = mem + p;
    while (len >= 2 && s[0] == '.' && s[1] == '/') { s += 2; len -= 2; }
    while (len && s[0] == '/') { s++; len--; }
    if (len == 0 || (len == 1 && s[0] == '.')) return 1;
    if (len > 40) return -W_NAMETOOLONG;
    for (u32 i = 0; i < len; i++) {
        u8 c = s[i];
        if (c == '/') return -W_NOENT;                                   /* в песочнице нет подкаталогов */
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok || (i == 0 && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))) return -W_NOTCAPABLE;
        out[i] = (char)c;
    }
    out[len] = 0; return 0;
}
static int wasi_err_fs(i64 r) {                                          /* ответ файловой службы -> errno */
    switch (r) {
    case -2: return W_NOENT; case -3: return W_EXIST; case -4: return W_NOTDIR; case -5: return W_ISDIR; case -6: return W_NOSPC;
    case -7: return W_INVAL; case -9: return W_FBIG; case -11: return W_NAMETOOLONG; case -13: return W_NOTCAPABLE;
    case -100: return W_NOTCAPABLE; case -101: return W_INVAL; case -102: return W_NOSPC; case -103: return W_AGAIN;
    default: return W_IO;
    }
}
static wfile_t* wfd(u32 fd) { return fd >= 4 && fd < 4 + WF_N && wf[fd - 4].used ? &wf[fd - 4] : 0; }

static int pool_take(void) {
    for (int i = 0; i < WF_POOL; i++) if (!wpool_used[i]) {
        if (!wpool[i]) { wpool[i] = (u8*)alloc(WF_CAP); if (!wpool[i]) return -1; }
        wpool_used[i] = 1; return i;
    }
    return -1;
}
static int wflush(wfile_t* f) {
    if (!f->wr || !f->dirty) return W_OK;
    i64 r = k_file_write(f->name, wpool[f->pool], f->size);
    if (r < 0) return wasi_err_fs(r);
    f->dirty = 0; return W_OK;
}
static void wfree(wfile_t* f) { if (f->wr) wpool_used[f->pool] = 0; f->used = 0; }

/* Чтение: n байт с позиции off в dst (память модуля). Результат - прочитано или -errno. */
static i64 wread_at(wfile_t* f, u8* dst, u64 n, u64 off) {
    if (f->dir) return -W_ISDIR;
    if (!n) return 0;
    if (f->wr) {
        if (off >= f->size) return 0;
        if (n > f->size - off) n = f->size - off;
        for (u64 i = 0; i < n; i++) dst[i] = wpool[f->pool][off + i];
        return (i64)n;
    }
    i64 r = k_file_read(f->name, dst, n, off);
    return r < 0 ? -(i64)wasi_err_fs(r) : r;
}
static i64 wwrite_at(wfile_t* f, const u8* src, u64 n, u64 off) {
    if (f->dir) return -W_ISDIR;
    if (!f->wr) return -W_BADF;
    if (off + n > WF_CAP || off + n < off) return -W_NOSPC;
    u8* b = wpool[f->pool];
    if (off > f->size) for (u64 i = f->size; i < off; i++) b[i] = 0;
    for (u64 i = 0; i < n; i++) b[off + i] = src[i];
    if (off + n > f->size) f->size = off + n;
    f->dirty = 1; return (i64)n;
}

/* Размер файла службой (имя из песочницы): >= 0 или -errno. */
static i64 wsize(const char* name) { i64 r = k_file_size(name); return r < 0 ? -(i64)wasi_err_fs(r) : r; }

static u64 wasi_filestat(u32 out, u32 type, u64 size, u64 lim) {
    if ((u64)out + 64 > lim) return W_FAULT;
    for (u32 i = 0; i < 64; i++) mem[out + i] = 0;
    mem[out + 16] = (u8)type; st64(mem + out + 24, 1); st64(mem + out + 32, size);
    return W_OK;
}

/* iov: считывает список (ptr,len) и проверяет границы заранее. */
static int wasi_iov_ok(u32 iov, u32 n, u64 lim, u64* total) {
    if (n > 1024 || (u64)iov + (u64)n * 8 > lim) return 0;
    u64 t = 0;
    for (u32 i = 0; i < n; i++) {
        u32 bp = ld32(mem + iov + 8 * i), bl = ld32(mem + iov + 8 * i + 4);
        if ((u64)bp + bl > lim) return 0;
        t += bl;
    }
    if (t > 0xFFFFFFFFull) return 0;
    *total = t; return 1;
}

static u64 wasi_read_common(u32 fd, u32 iov, u32 n, u64 off, int use_off, u32 out, u64 lim) {
    u64 total;
    if ((u64)out + 4 > lim) return W_FAULT;
    if (!wasi_iov_ok(iov, n, lim, &total)) return W_FAULT;
    if (fd == 0) {
        if (use_off) return W_SPIPE;
        for (u32 i = 0; i < n; i++) {
            u32 bp = ld32(mem + iov + 8 * i), bl = ld32(mem + iov + 8 * i + 4);
            if (!bl) continue;
            i64 r = k_stdin(mem + bp, bl > 4096 ? 4096 : bl);
            if (r == -4) return W_INTR;
            if (r < 0) return W_NOTSUP;                                   /* ввода у программы нет (право CAP_STDIN) или он занят другой */
            st32(mem + out, (u32)r); return W_OK;
        }
        st32(mem + out, 0); return W_OK;
    }
    wfile_t* f = wfd(fd);
    if (!f) return W_BADF;
    if (f->dir) return W_ISDIR;
    if (!f->rd) return W_BADF;
    u64 pos = use_off ? off : f->pos, got = 0;
    for (u32 i = 0; i < n; i++) {
        u32 bp = ld32(mem + iov + 8 * i), bl = ld32(mem + iov + 8 * i + 4);
        while (bl) {
            u32 c = bl > 65536 ? 65536 : bl;
            i64 r = wread_at(f, mem + bp, c, pos + got);
            if (r < 0) { if (got) goto done; return (u64)-r; }
            if (r == 0) goto done;
            got += (u64)r; bp += (u32)r; bl -= (u32)r;
            if ((u64)r < c) goto done;
        }
    }
done:
    if (!use_off) f->pos += got;
    st32(mem + out, (u32)got); return W_OK;
}
static u64 wasi_write_common(u32 fd, u32 iov, u32 n, u64 off, int use_off, u32 out, u64 lim) {
    u64 total;
    if ((u64)out + 4 > lim) return W_FAULT;
    if (!wasi_iov_ok(iov, n, lim, &total)) return W_FAULT;             /* сначала проверить все записи: частичный вывод при ошибке не допускается */
    if (fd == 1 || fd == 2) {
        if (use_off) return W_SPIPE;
        for (u32 i = 0; i < n; i++) {
            u32 bp = ld32(mem + iov + 8 * i), bl = ld32(mem + iov + 8 * i + 4);
            if (bl) k_write((const char*)mem + bp, bl);
        }
        st32(mem + out, (u32)total); return W_OK;
    }
    wfile_t* f = wfd(fd);
    if (!f || fd == 0) return W_BADF;
    if (f->dir) return W_ISDIR;
    if (!f->wr) return W_BADF;
    u64 pos = use_off ? off : (f->append ? f->size : f->pos), done = 0;
    for (u32 i = 0; i < n; i++) {
        u32 bp = ld32(mem + iov + 8 * i), bl = ld32(mem + iov + 8 * i + 4);
        i64 r = wwrite_at(f, mem + bp, bl, pos + done);
        if (r < 0) { if (done) break; return (u64)-r; }
        done += (u64)r;
    }
    if (!use_off) f->pos = pos + done;
    st32(mem + out, (u32)done); return W_OK;
}

static u64 wasi_path_open(const u64* a, u64 lim) {
    u32 dirfd = (u32)a[0], path = (u32)a[2], plen = (u32)a[3], oflags = (u32)a[4], fdflags = (u32)a[7], outp = (u32)a[8];
    u64 rights = a[5];
    if (dirfd != 3) return W_BADF;
    if ((u64)outp + 4 > lim) return W_FAULT;
    char name[44]; int nr = wasi_name(path, plen, lim, name);
    if (nr < 0) return (u64)-nr;
    int slot = -1;
    for (int i = 0; i < WF_N; i++) if (!wf[i].used) { slot = i; break; }
    if (slot < 0) return W_MFILE;
    wfile_t* f = &wf[slot];
    for (unsigned i = 0; i < sizeof *f; i++) ((u8*)f)[i] = 0;
    if (nr == 1) {                                                        /* сам каталог */
        if (oflags & 0x1 || (oflags & 0x8)) return W_ISDIR;
        f->used = 1; f->dir = 1; f->rd = 1; st32(mem + outp, 4 + (u32)slot); return W_OK;
    }
    if (oflags & 0x2) return W_NOTDIR;                                    /* O_DIRECTORY на обычное имя */
    int want_wr = (rights & ((1ull << 6) | (1ull << 22))) != 0 || (oflags & 0x8) != 0;
    int want_rd = (rights & ((1ull << 1) | (1ull << 14))) != 0 || !want_wr;
    i64 sz = k_file_size(name);
    int exists = sz >= 0;
    if (!exists && sz != -2) return wasi_err_fs(sz);                      /* ISDIR и прочее; -2 - просто нет файла */
    if (exists && (oflags & 0x1) && (oflags & 0x4)) return W_EXIST;
    if (!exists && !(oflags & 0x1)) return W_NOENT;
    f->rd = (u8)want_rd; f->wr = (u8)want_wr; f->append = (fdflags & 1) != 0;
    for (u32 i = 0; i < 44; i++) f->name[i] = name[i];
    f->size = exists ? (u64)sz : 0;
    if (want_wr) {
        int p = pool_take();
        if (p < 0) return W_MFILE;
        f->pool = (u8)p; f->wr = 1;
        if (exists && !(oflags & 0x8)) {
            if ((u64)sz > WF_CAP) { wpool_used[p] = 0; return W_NOSPC; }
            u64 got = 0;
            while (got < (u64)sz) {
                i64 r = k_file_read(name, wpool[p] + got, (u64)sz - got > 65536 ? 65536 : (u64)sz - got, got);
                if (r <= 0) { wpool_used[p] = 0; return r < 0 ? (u64)wasi_err_fs(r) : W_IO; }
                got += (u64)r;
            }
        } else f->size = 0;
        if (!exists || (oflags & 0x8)) {                                  /* новый или усечённый файл создаётся сразу: ошибка квоты видна при открытии */
            i64 r = k_file_write(name, "", 0);
            if (r < 0) { wpool_used[p] = 0; return wasi_err_fs(r); }
        }
    }
    f->used = 1;
    st32(mem + outp, 4 + (u32)slot); return W_OK;
}

static u64 wasi_poll(const u64* a, u64 lim) {
    u32 in = (u32)a[0], out = (u32)a[1], n = (u32)a[2], nev = (u32)a[3];
    if (!n) return W_INVAL;
    if (n > 16 || (u64)in + (u64)n * 48 > lim || (u64)out + (u64)n * 32 > lim || (u64)nev + 4 > lim) return W_FAULT;
    u64 now = k_clock_ns(), t0 = now, best = ~0ull;
    for (u32 i = 0; i < n; i++) {
        const u8* s = mem + in + 48 * i;
        if (s[8] != 0) return W_NOSYS;                                    /* ожидание готовности fd: нет (ввод консоли без опроса) */
        u64 to = ld64(s + 24); u16 fl = (u16)(s[40] | s[41] << 8);
        u64 when = (fl & 1) ? to : t0 + to;
        if (when < best) best = when;
    }
    while ((now = k_clock_ns()) < best) {
        u64 left = best - now, ms10 = left / 10000000ull;
        if (ms10 > 100) ms10 = 100;
        if (ms10) k_sleep(ms10); else k_yield();
        if (exit_req) break;
    }
    u32 ne = 0;
    for (u32 i = 0; i < n; i++) {                                         /* событие у каждого срока, который наступил (не позже ближайшего) */
        const u8* sb = mem + in + 48 * i;
        u64 to = ld64(sb + 24); u16 fl = (u16)(sb[40] | sb[41] << 8);
        u64 when = (fl & 1) ? to : t0 + to;
        if (when > best) continue;
        u8* e = mem + out + 32 * ne;
        for (u32 k = 0; k < 32; k++) e[k] = 0;
        st64(e, ld64(sb));                                                /* userdata; ошибка 0; тип clock (0) */
        ne++;
    }
    st32(mem + nev, ne); return W_OK;
}

static u64 wasi_call(u32 idx, const u64* a, u64 lim) {
    switch (idx) {
    case IW_proc_exit: exit_req = 1; exit_code = (int)a[0]; return 0;
    case IW_proc_raise: return W_NOSYS;
    case IW_sched_yield: k_yield(); return W_OK;
    case IW_clock_time_get: {
        u32 id = (u32)a[0], p = (u32)a[2];
        if ((u64)p + 8 > lim) return W_FAULT;
        if (id == 1) { st64(mem + p, k_clock_ns()); return W_OK; }          /* CLOCK_MONOTONIC */
        if (id == 0) { i64 t = k_time(); if (t < 0) return W_NOSYS; st64(mem + p, (u64)t * 1000000000ull); return W_OK; }   /* настенные: секунды из RTC */
        return W_NOSYS;
    }
    case IW_args_sizes_get: return (wput32((u32)a[0], wa_n, lim) && wput32((u32)a[1], wa_bytes, lim)) ? W_OK : W_FAULT;
    case IW_args_get: {
        u32 pv = (u32)a[0], pb = (u32)a[1];
        if ((u64)pv + 4ull * wa_n > lim || (u64)pb + wa_bytes > lim) return W_FAULT;
        for (u32 i = 0; i < wa_bytes; i++) mem[pb + i] = (u8)wa_buf[i];
        for (u32 i = 0; i < wa_n; i++) st32(mem + pv + 4 * i, pb + wa_off[i]);
        return W_OK;
    }
    case IW_environ_sizes_get: return (wput32((u32)a[0], 0, lim) && wput32((u32)a[1], 0, lim)) ? W_OK : W_FAULT;
    case IW_environ_get: return W_OK;
    case IW_random_get: {
        u32 p = (u32)a[0], n = (u32)a[1];
        if ((u64)p + n > lim) return W_FAULT;
        for (u32 off = 0; off < n; ) {
            u32 c = n - off > 256 ? 256 : n - off;
            if (k_random(mem + p + off, c) < 0) return W_NOSYS;          /* RNG недоступен: честный отказ, а не предсказуемые байты */
            off += c;
        }
        return W_OK;
    }
    case IW_fd_write: return wasi_write_common((u32)a[0], (u32)a[1], (u32)a[2], 0, 0, (u32)a[3], lim);
    case IW_fd_pwrite: return wasi_write_common((u32)a[0], (u32)a[1], (u32)a[2], a[3], 1, (u32)a[4], lim);
    case IW_fd_read: return wasi_read_common((u32)a[0], (u32)a[1], (u32)a[2], 0, 0, (u32)a[3], lim);
    case IW_fd_pread: return wasi_read_common((u32)a[0], (u32)a[1], (u32)a[2], a[3], 1, (u32)a[4], lim);
    case IW_fd_close: {
        u32 fd = (u32)a[0];
        if (fd <= 3) return W_OK;
        wfile_t* f = wfd(fd); if (!f) return W_BADF;
        int e = wflush(f); wfree(f); return (u64)e;
    }
    case IW_fd_sync: case IW_fd_datasync: {
        u32 fd = (u32)a[0];
        if (fd <= 3) return W_OK;
        wfile_t* f = wfd(fd); if (!f) return W_BADF;
        return (u64)wflush(f);
    }
    case IW_fd_seek: case IW_fd_tell: {
        u32 fd = (u32)a[0]; wfile_t* f = wfd(fd);
        if (fd <= 3) return W_SPIPE;
        if (!f) return W_BADF;
        if (f->dir) return W_ISDIR;
        u32 outp; i64 np = (i64)f->pos;
        if (idx == IW_fd_seek) {
            i64 off = (i64)a[1]; u32 wh = (u32)a[2]; outp = (u32)a[3];
            i64 base = wh == 0 ? 0 : wh == 1 ? (i64)f->pos : -1;
            if (wh == 2) { if (f->wr) base = (i64)f->size; else { i64 sz = wsize(f->name); if (sz < 0) return (u64)-sz; base = sz; } }
            if (base < 0 || wh > 2) return W_INVAL;
            np = base + off;
            if (np < 0) return W_INVAL;
            if (!wput64(outp, (u64)np, lim)) return W_FAULT;
            f->pos = (u64)np; return W_OK;
        }
        outp = (u32)a[1];
        return wput64(outp, f->pos, lim) ? W_OK : W_FAULT;
    }
    case IW_fd_fdstat_get: {
        u32 fd = (u32)a[0], p = (u32)a[1];
        if ((u64)p + 24 > lim) return W_FAULT;
        for (u32 i = 0; i < 24; i++) mem[p + i] = 0;
        if (fd <= 2) { mem[p] = WT_CHR; st64(mem + p + 8, fd == 0 ? (1ull << 1) : (1ull << 6)); return W_OK; }
        if (fd == 3) { mem[p] = WT_DIR; st64(mem + p + 8, ~0ull >> 1); st64(mem + p + 16, ~0ull >> 1); return W_OK; }
        wfile_t* f = wfd(fd); if (!f) return W_BADF;
        if (f->dir) { mem[p] = WT_DIR; st64(mem + p + 8, ~0ull >> 1); st64(mem + p + 16, ~0ull >> 1); return W_OK; }
        mem[p] = WT_REG; mem[p + 2] = f->append ? 1 : 0;
        st64(mem + p + 8, (1ull << 2) | (1ull << 4) | (1ull << 5) | (1ull << 21) | (f->rd ? 1ull << 1 : 0) | (f->wr ? (1ull << 6) | (1ull << 3) | (1ull << 22) : 0));
        return W_OK;
    }
    case IW_fd_fdstat_set_flags: {
        wfile_t* f = wfd((u32)a[0]); if (!f) return (u32)a[0] <= 3 ? W_NOTSUP : W_BADF;
        if ((u32)a[1] & ~1u) return W_NOTSUP;                               /* только APPEND */
        f->append = ((u32)a[1] & 1) != 0; return W_OK;
    }
    case IW_fd_filestat_get: {
        u32 fd = (u32)a[0], p = (u32)a[1];
        if (fd <= 2) return wasi_filestat(p, WT_CHR, 0, lim);
        if (fd == 3) return wasi_filestat(p, WT_DIR, 0, lim);
        wfile_t* f = wfd(fd); if (!f) return W_BADF;
        if (f->dir) return wasi_filestat(p, WT_DIR, 0, lim);
        if (f->wr) return wasi_filestat(p, WT_REG, f->size, lim);
        i64 sz = wsize(f->name); if (sz < 0) return (u64)-sz;
        return wasi_filestat(p, WT_REG, (u64)sz, lim);
    }
    case IW_fd_filestat_set_size: {
        wfile_t* f = wfd((u32)a[0]); if (!f) return W_BADF;
        if (!f->wr) return W_BADF;
        u64 n = a[1]; if (n > WF_CAP) return W_FBIG;
        for (u64 i = f->size; i < n; i++) wpool[f->pool][i] = 0;
        f->size = n; f->dirty = 1; return W_OK;
    }
    case IW_fd_prestat_get: {
        u32 fd = (u32)a[0], p = (u32)a[1];
        if (fd != 3) return W_BADF;
        if ((u64)p + 8 > lim) return W_FAULT;
        st32(mem + p, 0); st32(mem + p + 4, 1); return W_OK;               /* тип «каталог», имя "/" */
    }
    case IW_fd_prestat_dir_name: {
        u32 fd = (u32)a[0], p = (u32)a[1], n = (u32)a[2];
        if (fd != 3) return W_BADF;
        if (n < 1) return W_INVAL;
        if ((u64)p + 1 > lim) return W_FAULT;
        mem[p] = '/'; return W_OK;
    }
    case IW_fd_readdir: {
        u32 fd = (u32)a[0], buf = (u32)a[1], blen = (u32)a[2], used_p = (u32)a[4]; u64 cookie = a[3];
        wfile_t* f = wfd(fd);
        if (fd != 3 && !(f && f->dir)) return fd <= 3 || f ? W_NOTDIR : W_BADF;
        if ((u64)buf + blen > lim || (u64)used_p + 4 > lim) return W_FAULT;
        if (!wlist) { wlist = (char*)alloc(wlist_cap); if (!wlist) return W_NOSPC; }
        i64 n = k_file_list(wlist, wlist_cap);
        if (n < 0) return (u64)wasi_err_fs(n);
        u32 out = 0, idx2 = 0;
        for (i64 i = 0; i < n; ) {
            i64 e = i; while (e < n && wlist[e] != '\n') e++;
            u32 nl = (u32)(e - i);
            if (idx2 >= cookie) {
                u8 hdr[24]; for (int k = 0; k < 24; k++) hdr[k] = 0;
                u64 nx = (u64)idx2 + 1; for (int k = 0; k < 8; k++) hdr[k] = (u8)(nx >> (8 * k));
                u64 ino = (u64)idx2 + 2; for (int k = 0; k < 8; k++) hdr[8 + k] = (u8)(ino >> (8 * k));
                for (int k = 0; k < 4; k++) hdr[16 + k] = (u8)(nl >> (8 * k));
                hdr[20] = WT_REG;
                for (u32 k = 0; k < 24 && out < blen; k++) mem[buf + out++] = hdr[k];
                for (u32 k = 0; k < nl && out < blen; k++) mem[buf + out++] = (u8)wlist[i + k];
                if (out >= blen) break;                                   /* буфер полон: модуль повторит с новым cookie */
            }
            idx2++; i = e + 1;
        }
        st32(mem + used_p, out); return W_OK;
    }
    case IW_path_open: return wasi_path_open(a, lim);
    case IW_path_filestat_get: {
        if ((u32)a[0] != 3) return W_BADF;
        char name[44]; int nr = wasi_name((u32)a[2], (u32)a[3], lim, name);
        if (nr < 0) return (u64)-nr;
        if (nr == 1) return wasi_filestat((u32)a[4], WT_DIR, 0, lim);
        i64 sz = k_file_size(name);
        if (sz < 0) return sz == -5 ? wasi_filestat((u32)a[4], WT_DIR, 0, lim) : (u64)wasi_err_fs(sz);
        return wasi_filestat((u32)a[4], WT_REG, (u64)sz, lim);
    }
    case IW_path_unlink_file: {
        if ((u32)a[0] != 3) return W_BADF;
        char name[44]; int nr = wasi_name((u32)a[1], (u32)a[2], lim, name);
        if (nr < 0) return (u64)-nr;
        if (nr == 1) return W_ISDIR;
        i64 r = k_file_remove(name);
        return r < 0 ? (u64)wasi_err_fs(r) : W_OK;
    }
    case IW_path_create_directory: return (u32)a[0] == 3 ? W_NOTSUP : W_BADF;      /* песочница плоская */
    case IW_path_remove_directory: return (u32)a[0] == 3 ? W_NOTSUP : W_BADF;
    case IW_path_rename: {
        if ((u32)a[0] != 3 || (u32)a[3] != 3) return W_BADF;
        char from[44], to[44];
        int n1 = wasi_name((u32)a[1], (u32)a[2], lim, from), n2 = wasi_name((u32)a[4], (u32)a[5], lim, to);
        if (n1 < 0) return (u64)-n1;
        if (n2 < 0) return (u64)-n2;
        if (n1 == 1 || n2 == 1) return W_NOTSUP;
        i64 sz = wsize(from); if (sz < 0) return (u64)-sz;
        if ((u64)sz > WF_CAP) return W_NOSPC;
        int p = pool_take(); if (p < 0) return W_MFILE;
        u64 got = 0; int err = 0;
        while (got < (u64)sz) {
            i64 r = k_file_read(from, wpool[p] + got, (u64)sz - got > 65536 ? 65536 : (u64)sz - got, got);
            if (r <= 0) { err = r < 0 ? wasi_err_fs(r) : W_IO; break; }
            got += (u64)r;
        }
        if (!err) { i64 r = k_file_write(to, wpool[p], (u64)sz); if (r < 0) err = wasi_err_fs(r); }
        wpool_used[p] = 0;
        if (err) return (u64)err;
        { int same = 1; for (u32 i = 0; i < 44 && same; i++) { if (from[i] != to[i]) same = 0; if (!from[i]) break; } if (same) return W_OK; }
        i64 r = k_file_remove(from);
        return r < 0 ? (u64)wasi_err_fs(r) : W_OK;
    }
    case IW_poll_oneoff: return wasi_poll(a, lim);
    default: return W_NOSYS;                                              /* fd_advise, fd_allocate, fd_renumber, ссылки, права, времена, сокеты, clock_res_get */
    }
}
#endif
