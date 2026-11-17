/* Проверка файлового WASI, ввода, аргументов и ожидания в интерпретаторе wasm (модуль - обычный клиент wasm32-wasi без libc).
   Хост даёт плоскую песочницу в памяти, строки ввода WASM_STDIN и аргументы WASM_ARGS. Вывод сверяется в tests/run_host_tests.sh. */
#define W(n) __attribute__((import_module("wasi_snapshot_preview1"), import_name(n)))
typedef unsigned char u8; typedef long long i64; typedef unsigned long long u64;
typedef struct { const void* p; int n; } iov_t;
W("fd_write") int fd_write(int fd, const iov_t* iov, int n, int* out);
W("fd_read") int fd_read(int fd, const iov_t* iov, int n, int* out);
W("fd_pread") int fd_pread(int fd, const iov_t* iov, int n, i64 off, int* out);
W("fd_close") int fd_close(int fd);
W("fd_seek") int fd_seek(int fd, i64 off, int whence, i64* out);
W("fd_tell") int fd_tell(int fd, i64* out);
W("fd_sync") int fd_sync(int fd);
W("fd_fdstat_get") int fd_fdstat_get(int fd, void* st);
W("fd_filestat_get") int fd_filestat_get(int fd, void* st);
W("fd_filestat_set_size") int fd_filestat_set_size(int fd, i64 n);
W("fd_prestat_get") int fd_prestat_get(int fd, void* p);
W("fd_prestat_dir_name") int fd_prestat_dir_name(int fd, char* p, int n);
W("fd_readdir") int fd_readdir(int fd, void* buf, int n, i64 cookie, int* used);
W("fd_allocate") int fd_allocate(int fd, i64 off, i64 len);
W("path_open") int path_open(int dir, int dflags, const char* p, int pl, int of, i64 rb, i64 ri, int ff, int* fd);
W("path_filestat_get") int path_filestat_get(int dir, int fl, const char* p, int pl, void* st);
W("path_unlink_file") int path_unlink_file(int dir, const char* p, int pl);
W("path_create_directory") int path_create_directory(int dir, const char* p, int pl);
W("path_rename") int path_rename(int d1, const char* a, int al, int d2, const char* b, int bl);
W("poll_oneoff") int poll_oneoff(const void* in, void* out, int n, int* ne);
W("args_sizes_get") int args_sizes_get(int* argc, int* sz);
W("args_get") int args_get(char** argv, char* buf);
W("proc_exit") void proc_exit(int code);

enum { CREAT = 1, DIRECTORY = 2, EXCL = 4, TRUNC = 8 };
#define RD ((i64)1 << 1)
#define WR ((i64)1 << 6)
static int len(const char* s) { int n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { iov_t v = { s, len(s) }; int w; fd_write(1, &v, 1, &w); }
static void num(const char* label, i64 v) {
    char b[48]; int n = 0, neg = v < 0; u64 u = neg ? -(u64)v : (u64)v; char t[24]; int k = 0;
    do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[n++] = '-';
    while (k) b[n++] = t[--k];
    b[n++] = '\n'; b[n] = 0;
    out(label); out(b);
}
static void str(const char* label, const char* s, int n) { out(label); iov_t v = { s, n }; int w; fd_write(1, &v, 1, &w); out("\n"); }
static int opn(const char* p, int of, i64 rights, int ff, int* fd) { return path_open(3, 0, p, len(p), of, rights, 0, ff, fd); }
static int wr(int fd, const char* s) { iov_t v = { s, len(s) }; int w = -1; int rc = fd_write(fd, &v, 1, &w); return rc ? -rc : w; }
static int rd(int fd, char* b, int n) { iov_t v = { b, n }; int r = -1; int rc = fd_read(fd, &v, 1, &r); return rc ? -rc : r; }

static char big[1024];
int _start(void) {
    char b[64]; int fd = -1, fd2 = -1; i64 pos; u8 st[64];
    /* преоткрытый каталог */
    u8 pre[8]; num("prestat fd3 rc ", fd_prestat_get(3, pre)); num("prestat tag ", pre[0]); num("prestat name len ", pre[4]);
    num("prestat fd4 rc ", fd_prestat_get(4, pre));                                 /* 8 (BADF) */
    char dn[2] = { 0, 0 }; num("prestat dir name rc ", fd_prestat_dir_name(3, dn, 1)); str("prestat dir name ", dn, 1);
    /* создание, запись, закрытие */
    num("open missing rc ", opn("a.txt", 0, RD, 0, &fd));                           /* 44 (NOENT) */
    num("create rc ", opn("a.txt", CREAT | TRUNC, WR, 0, &fd)); num("create fd ", fd);
    num("write ", wr(fd, "hello world")); num("fsync rc ", fd_sync(fd)); num("close rc ", fd_close(fd));
    num("write on closed rc ", -wr(fd, "x"));                                       /* 8 */
    /* чтение, seek, tell, pread */
    num("open read rc ", opn("a.txt", 0, RD, 0, &fd));
    num("read ", rd(fd, b, 5)); str("read data ", b, 5);
    num("tell rc ", fd_tell(fd, &pos)); num("tell ", pos);
    num("seek end rc ", fd_seek(fd, 0, 2, &pos)); num("seek end pos ", pos);
    num("seek set rc ", fd_seek(fd, 6, 0, &pos)); num("read ", rd(fd, b, 32)); str("read data ", b, 5);
    num("read at eof ", rd(fd, b, 32));
    { iov_t v = { b, 5 }; int r = -1; num("pread rc ", fd_pread(fd, &v, 1, 0, &r)); num("pread n ", r); str("pread data ", b, 5); }
    num("seek negative rc ", fd_seek(fd, -100, 1, &pos));                           /* 28 (INVAL) */
    num("write to read-only fd rc ", -wr(fd, "x"));                                 /* 8 */
    u8 fs[24]; num("fdstat rc ", fd_fdstat_get(fd, fs)); num("fdstat filetype ", fs[0]);
    num("filestat rc ", fd_filestat_get(fd, st)); num("filestat type ", st[16]); num("filestat size ", st[32]);
    num("close rc ", fd_close(fd));
    /* дозапись, усечение */
    num("open append rc ", opn("a.txt", 0, WR, 1, &fd)); num("append write ", wr(fd, "!!")); num("close rc ", fd_close(fd));
    num("path_filestat rc ", path_filestat_get(3, 0, "a.txt", 5, st)); num("size after append ", st[32]);
    num("open rw rc ", opn("a.txt", 0, RD | WR, 0, &fd)); num("rw read ", rd(fd, b, 5)); str("rw data ", b, 5);
    num("overwrite at pos ", wr(fd, "HELLO")); num("set size rc ", fd_filestat_set_size(fd, 4)); num("close rc ", fd_close(fd));
    num("open read rc ", opn("a.txt", 0, RD, 0, &fd)); num("read ", rd(fd, b, 32)); str("data ", b, 4); fd_close(fd);
    /* имена и ошибки */
    num("excl existing rc ", opn("a.txt", CREAT | EXCL, WR, 0, &fd));               /* 20 (EXIST) */
    num("subdir name rc ", opn("d/x.txt", CREAT, WR, 0, &fd));                      /* 44 */
    num("dotdot name rc ", opn("../x", 0, RD, 0, &fd));                             /* 76 (NOTCAPABLE) */
    num("bad char rc ", opn("a b", CREAT, WR, 0, &fd));                             /* 76 */
    num("long name rc ", opn("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", CREAT, WR, 0, &fd));   /* 37 (NAMETOOLONG) */
    num("not a dir rc ", opn("a.txt", DIRECTORY, RD, 0, &fd));                      /* 54 (NOTDIR) */
    num("mkdir rc ", path_create_directory(3, "sub", 3));                           /* 58 (NOTSUP) */
    num("bad dirfd rc ", path_filestat_get(5, 0, "a.txt", 5, st));                  /* 8 */
    num("fd_allocate rc ", fd_allocate(3, 0, 10));                                  /* 52 (NOSYS): не поддержано честно */
    /* каталог */
    num("open dir rc ", opn(".", 0, RD, 0, &fd)); num("dir fd ", fd);
    num("dir fdstat rc ", fd_fdstat_get(fd, fs)); num("dir fdstat type ", fs[0]);
    num("read on dir rc ", -rd(fd, b, 4));                                          /* 31 (ISDIR) */
    { int used = -1; char de[128]; num("readdir rc ", fd_readdir(fd, de, sizeof de, 0, &used)); num("readdir used ", used);
      num("dirent namlen ", de[16]); num("dirent type ", de[20]); str("dirent name ", de + 24, 5);
      num("readdir after last rc ", fd_readdir(fd, de, sizeof de, 1, &used)); num("readdir after last used ", used); }
    fd_close(fd);
    /* rename, unlink */
    num("rename rc ", path_rename(3, "a.txt", 5, 3, "b.txt", 5)); num("old gone rc ", opn("a.txt", 0, RD, 0, &fd));
    num("new there rc ", path_filestat_get(3, 0, "b.txt", 5, st)); num("new size ", st[32]);
    num("unlink rc ", path_unlink_file(3, "b.txt", 5)); num("unlink again rc ", path_unlink_file(3, "b.txt", 5));   /* 44 */
    /* предел размера и число файлов на запись */
    num("open big rc ", opn("big.bin", CREAT | TRUNC, WR, 0, &fd));
    { int total = 0, rc = 0; for (int i = 0; i < 1024; i++) big[i] = (char)i;
      for (int i = 0; i < 520; i++) { iov_t v = { big, 1024 }; int w; rc = fd_write(fd, &v, 1, &w); if (rc) break; total += w; }
      num("big written ", total); num("big stop rc ", rc);                         /* 524288 и 51 (NOSPC) */ }
    num("close big rc ", fd_close(fd));
    { int f[5]; int k;
      for (k = 0; k < 4; k++) { char nm[8] = { 'p', (char)('0' + k), 0 }; if (opn(nm, CREAT | TRUNC, WR, 0, &f[k])) break; }
      num("pool opened ", k); num("pool 5th rc ", opn("p9", CREAT | TRUNC, WR, 0, &f[4]));    /* 33 (MFILE) */
      for (int i = 0; i < k; i++) fd_close(f[i]); }
    /* ввод */
    num("stdin 1 ", rd(0, b, 64)); str("stdin data ", b, 5);
    num("stdin 2 ", rd(0, b, 3)); str("stdin part ", b, 3); num("stdin 2 rest ", rd(0, b, 64));
    num("stdin eof ", rd(0, b, 64));
    /* аргументы */
    { int argc = -1, sz = -1; num("args rc ", args_sizes_get(&argc, &sz)); num("argc ", argc);
      char* av[8]; char ab[256]; num("args_get rc ", args_get(av, ab)); str("argv1 ", av[1], len(av[1])); str("argv2 ", av[2], len(av[2])); }
    /* ожидание по часам: подписка на 10 мс */
    { u8 sub[48], ev[32]; for (int i = 0; i < 48; i++) sub[i] = 0; for (int i = 0; i < 32; i++) ev[i] = 0;
      sub[0] = 42; sub[8] = 0; sub[16] = 1; *(u64*)(sub + 24) = 10000000ull; int ne = -1;
      num("poll rc ", poll_oneoff(sub, ev, 1, &ne)); num("poll events ", ne); num("poll userdata ", ev[0]); num("poll error ", ev[8]);
      sub[8] = 1; num("poll fd_read rc ", poll_oneoff(sub, ev, 1, &ne)); }       /* 52: опрос fd не поддержан */
    out("wasifs: done\n");
    proc_exit(0);
    return 0;
}
