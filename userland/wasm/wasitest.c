/* Проверка подмножества WASI 0.1 (wasi_snapshot_preview1) в интерпретаторе wasm: модуль собран как обычный wasm32-wasi клиент.
   Вывод сверяется в tests/run_host_tests.sh. */
#define W(n) __attribute__((import_module("wasi_snapshot_preview1"), import_name(n)))
W("fd_write") int fd_write(int fd, const void* iov, int n, int* out);
W("fd_close") int fd_close(int fd);
W("fd_seek") int fd_seek(int fd, long long off, int whence, long long* out);
W("fd_fdstat_get") int fd_fdstat_get(int fd, void* st);
W("clock_time_get") int clock_time_get(int id, long long prec, long long* out);
W("args_sizes_get") int args_sizes_get(int* argc, int* sz);
W("environ_sizes_get") int environ_sizes_get(int* n, int* sz);
W("random_get") int random_get(void* p, int n);
W("proc_exit") void proc_exit(int code);

typedef struct { const char* p; int n; } iov_t;
static int len(const char* s) { int n = 0; while (s[n]) n++; return n; }
static void out(const char* s) { iov_t v = { s, len(s) }; int w; fd_write(1, &v, 1, &w); }
static void num(const char* label, long long v) {
    char b[48]; int n = 0, neg = v < 0; unsigned long long u = neg ? -(unsigned long long)v : (unsigned long long)v; char t[24]; int k = 0;
    do { t[k++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[n++] = '-';
    while (k) b[n++] = t[--k];
    b[n++] = '\n'; b[n] = 0;
    out(label); out(b);
}

int _start(void) {
    /* два буфера за один вызов: порядок и сумма длин */
    iov_t two[2] = { { "wasi: ", 6 }, { "fd_write two buffers\n", 21 } };
    int w = -1;
    num("fd_write rc ", fd_write(1, two, 2, &w)); num("fd_write bytes ", w);
    num("bad fd rc ", fd_write(7, two, 1, &w));                       /* ожидается 8 (BADF) */
    num("fault rc ", fd_write(1, (void*)0xFFFFFF00, 1, &w));          /* вне памяти: 21 (FAULT), частичного вывода нет */
    long long t0 = 0, t1 = 0;
    num("clock monotonic rc ", clock_time_get(1, 1, &t0)); clock_time_get(1, 1, &t1);
    num("clock monotonic non-decreasing ", t1 >= t0);
    num("clock realtime rc ", clock_time_get(0, 1, &t0)); num("clock realtime plausible ", t0 > 1000000000ll * 1000000000ll);   /* RTC, секунды */
    num("clock unknown id rc ", clock_time_get(5, 1, &t0));          /* 52 (NOSYS) */
    int argc = -1, asz = -1; num("args rc ", args_sizes_get(&argc, &asz)); num("argc ", argc); num("argv size positive ", asz > 0);
    int ne = -1, esz = -1; num("environ rc ", environ_sizes_get(&ne, &esz)); num("environ n ", ne);
    unsigned char st[24]; num("fdstat rc ", fd_fdstat_get(1, st)); num("fdstat filetype ", st[0]);
    long long pos; num("seek stdout rc ", fd_seek(1, 0, 0, &pos));    /* 70 (SPIPE) */
    num("close stdout rc ", fd_close(1)); num("close fd 9 rc ", fd_close(9));
    unsigned char r1[32], r2[32]; for (int i = 0; i < 32; i++) r1[i] = r2[i] = 0;
    num("random rc ", random_get(r1, 32)); random_get(r2, 32);
    int diff = 0, nz = 0; for (int i = 0; i < 32; i++) { diff |= r1[i] != r2[i]; nz |= r1[i]; }
    num("random bytes differ between calls ", diff); num("random not all zero ", nz != 0);
    num("random fault rc ", random_get((void*)0xFFFFFF00, 64));       /* 21 (FAULT) */
    out("wasi: done\n");
    proc_exit(3);
    out("wasi: NOT REACHED\n");
    return 0;
}
