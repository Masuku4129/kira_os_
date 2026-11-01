/* Хост-загрузчик ELF Киры для тестов kcc (Linux x86-64).
 * Образ проверяется настоящим elf_parse() ядра (окно пользователя, W^X, выравнивание, точка входа), сегменты отображаются
 * по их виртуальным адресам с правами R-X / R-- / RW-, затем управление передаётся в точку входа.
 * Программа Киры делает инструкцию syscall с номерами Киры; seccomp(SECCOMP_RET_TRAP) перехватывает именно syscall из
 * окна адресов 0x8000000000..0x8fffffffff (код загруженной программы) и обрабатывается таблицей k_* ниже. Остальные вызовы хоста не затрагиваются.
 * Печатает "<код выхода>" программы так же, как эталон: printf("%d\n", main()).
 * Для проверки kcc, собранного как программа Киры (userland/kcc.c): kloader [-s песочница] [-i каталог заголовков] [-a "аргументы"] prog.elf
 * K_FS (ops 1 READ, 2 WRITE, 3 STAT песочницы и 11 INC_READ, 12 INC_STAT) и K_ARGS обслуживаются каталогами хоста по тем же правилам имён, что fsrv.c. */
#define _GNU_SOURCE
#include "../../elf.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define USER_LO 0x8000000000ULL
#define USER_HI 0x800000000000ULL
#define HEAP_BASE 0x8800000000ULL
#define HEAP_MAX_PAGES 1024

enum { K_WRITE = 1, K_EXIT = 2, K_TICKS = 3, K_SLEEP = 4, K_YIELD = 5, K_GETPID = 6, K_FS = 10, K_HEAP = 11, K_ARGS = 30 };

static unsigned long heap_pages;
static const char *sandbox_dir, *inc_dir, *prog_args = "";
typedef struct { uint32_t op, name_len; uint64_t name_ptr, buf_ptr, buf_len, off; } fsreq_t;

static int name_ok(const char *n, uint32_t len) {
    if (len == 0 || len > 40) return 0;
    for (uint32_t i = 0; i < len; i++) {
        char c = n[i];
        int alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (i == 0 ? !alnum : !(alnum || c == '.' || c == '-' || c == '_')) return 0;
    }
    return 1;
}

static long fs_op(const fsreq_t *q) {
    char path[256], name[48];
    if (!name_ok((const char *)q->name_ptr, q->name_len)) return -100;
    memcpy(name, (const void *)q->name_ptr, q->name_len); name[q->name_len] = 0;
    int inc = q->op == 11 || q->op == 12;
    const char *dir = inc ? inc_dir : sandbox_dir;
    if (!dir || (q->op != 1 && q->op != 2 && q->op != 3 && !inc)) return -101;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (q->op == 2) {
        FILE *f = fopen(path, "wb");
        if (!f) return -6;
        size_t w = q->buf_len ? fwrite((const void *)q->buf_ptr, 1, q->buf_len, f) : 0;
        fclose(f);
        return w == q->buf_len ? 0 : -6;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return -2;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    long r = sz;
    if (q->op == 1 || q->op == 11) {
        if (q->buf_len == 0) r = -101;
        else if (fseek(f, (long)q->off, SEEK_SET) != 0) r = -101;
        else r = (long)fread((void *)q->buf_ptr, 1, q->buf_len, f);
    }
    fclose(f);
    return r;
}

static void on_sys(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    ucontext_t *uc = ucv;
    greg_t *g = uc->uc_mcontext.gregs;
    unsigned long n = (unsigned long)g[REG_RAX], a = (unsigned long)g[REG_RDI], b = (unsigned long)g[REG_RSI];
    long r = 0;
    switch (n) {
    case K_WRITE: r = write(1, (const void *)a, b); break;
    case K_EXIT: fflush(stdout); printf("%d\n", (int)a); fflush(stdout); _exit(0);
    case K_TICKS: { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); r = ts.tv_sec * 100 + ts.tv_nsec / 10000000; break; }
    case K_SLEEP: { struct timespec ts = { (time_t)(a / 100), (long)(a % 100) * 10000000L }; nanosleep(&ts, NULL); break; }
    case K_YIELD: sched_yield(); break;
    case K_GETPID: r = 1; break;
    case K_FS: r = fs_op((const fsreq_t *)a); break;
    case K_ARGS: { size_t n = strlen(prog_args); if (n + 1 > b) { r = -1; break; } memcpy((void *)a, prog_args, n + 1); r = (long)n; break; }
    case K_HEAP: {
        if (a == 0 || heap_pages + a > HEAP_MAX_PAGES) { r = -1; break; }
        void *p = mmap((void *)(HEAP_BASE + heap_pages * 4096), a * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) { r = -1; break; }
        r = (long)p; heap_pages += a;
        break;
    }
    default: {
        char m[96];
        int k = snprintf(m, sizeof m, "kloader: unsupported Kira syscall %lu\n", n);
        if (write(2, m, (size_t)k) < 0) _exit(99);
        _exit(98);
    }
    }
    g[REG_RAX] = r;
}

static int install_seccomp(void) {
    struct sock_filter f[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, instruction_pointer) + 4),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 0x80, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog p = { sizeof f / sizeof f[0], f };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) return -1;
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &p);
}

int main(int argc, char **argv) {
    while (argc > 2 && argv[1][0] == '-' && argv[1][1] && argv[1][2] == 0 && argc > 3) {
        if (argv[1][1] == 's') sandbox_dir = argv[2];
        else if (argv[1][1] == 'i') inc_dir = argv[2];
        else if (argv[1][1] == 'a') prog_args = argv[2];
        else break;
        argv += 2; argc -= 2;
    }
    if (argc < 2) { fprintf(stderr, "usage: kloader [-s dir] [-i dir] [-a args] prog.elf\n"); return 2; }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror(argv[1]); return 2; }
    off_t len = lseek(fd, 0, SEEK_END);
    if (len <= 0 || len > (1 << 28)) { fprintf(stderr, "kloader: bad file size\n"); return 2; }
    unsigned char *img = malloc((size_t)len);
    if (!img || pread(fd, img, (size_t)len, 0) != len) { fprintf(stderr, "kloader: read error\n"); return 2; }
    close(fd);
    elf_info_t info;
    int e = elf_parse(img, (size_t)len, USER_LO, USER_HI, 4096, &info);
    if (e != ELF_OK) { fprintf(stderr, "kloader: elf_parse: %s\n", elf_strerror(e)); return 3; }
    for (unsigned i = 0; i < info.nsegs; i++) {
        const elf_seg_t *s = &info.seg[i];
        size_t mlen = (size_t)((s->mem_size + 4095) & ~4095ULL);
        void *p = mmap((void *)s->vaddr, mlen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED || (uint64_t)p != s->vaddr) { fprintf(stderr, "kloader: cannot map segment at 0x%llx: %s\n", (unsigned long long)s->vaddr, strerror(errno)); return 3; }
        memcpy(p, img + s->file_off, (size_t)s->file_size);
        if (mprotect(p, mlen, PROT_READ | (s->write ? PROT_WRITE : 0) | (s->exec ? PROT_EXEC : 0))) { perror("mprotect"); return 3; }
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sys;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSYS, &sa, NULL);
    if (install_seccomp()) { perror("seccomp"); return 3; }
    fflush(stdout);
    ((void (*)(void))info.entry)();
    fprintf(stderr, "kloader: entry returned\n");
    return 4;
}
