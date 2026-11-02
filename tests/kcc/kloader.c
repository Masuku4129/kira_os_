/* Хост-загрузчик ELF Киры для тестов kcc (Linux x86-64).
 * Образ проверяется настоящим elf_parse() ядра (окно пользователя, W^X, выравнивание, точка входа), сегменты отображаются
 * по их виртуальным адресам с правами R-X / R-- / RW-, затем управление передаётся в точку входа.
 * Программа Киры делает инструкцию syscall с номерами Киры; seccomp(SECCOMP_RET_TRAP) перехватывает именно syscall из
 * окна адресов 0x8000000000..0x8fffffffff (код загруженной программы) и обрабатывается таблицей k_* ниже. Остальные вызовы хоста не затрагиваются.
 * Печатает "<код выхода>" программы так же, как эталон: printf("%d\n", main()).
 * Для проверки kcc, собранного как программа Киры (userland/kcc.c): kloader [-s песочница] [-i каталог заголовков] [-a "аргументы"] prog.elf
 * -y каталог (=/etc/ssl, ops 9/10), -d каталог (=/drv, ops 13..17 FS_OP_DRV_*), сеть K_NET_* на сокетах хоста, K_TIME/K_RANDOM/K_CLOCK_NS (для drvget).
 * K_FS (ops 1 READ, 2 WRITE, 3 STAT, 4 LIST, 5 REMOVE песочницы и 11 INC_READ, 12 INC_STAT) и K_ARGS обслуживаются каталогами хоста по тем же правилам имён, что fsrv.c. */
#define _GNU_SOURCE
#include "../../elf.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
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
#include <sys/random.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define USER_LO 0x8000000000ULL
#define USER_HI 0x800000000000ULL
#define HEAP_BASE 0x8800000000ULL
#define HEAP_MAX_PAGES 1024

enum { K_WRITE = 1, K_EXIT = 2, K_TICKS = 3, K_SLEEP = 4, K_YIELD = 5, K_GETPID = 6, K_FS = 10, K_HEAP = 11, K_CLOCK_NS = 24, K_NET_DNS = 25, K_NET_CONNECT = 26,
       K_NET_SEND = 27, K_NET_RECV = 28, K_NET_CLOSE = 29, K_ARGS = 30, K_RANDOM = 36, K_TIME = 37 };

static unsigned long heap_pages;
static const char *sandbox_dir, *inc_dir, *sys_dir, *drv_dir, *prog_args = "";
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
    if (q->op == 4 || q->op == 17) {                     /* LIST песочницы (4) и каталога /drv (17): имена файлов через перевод строки */
        const char *dir = q->op == 4 ? sandbox_dir : drv_dir;
        if (!dir) return -101;
        DIR *d = opendir(dir);
        if (!d) return -2;
        size_t used = 0;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            size_t n = strlen(e->d_name);
            if (used + n + 1 > q->buf_len) break;
            memcpy((char *)q->buf_ptr + used, e->d_name, n); used += n; ((char *)q->buf_ptr)[used++] = '\n';
        }
        closedir(d);
        return (long)used;
    }
    if (!name_ok((const char *)q->name_ptr, q->name_len)) return -100;
    memcpy(name, (const void *)q->name_ptr, q->name_len); name[q->name_len] = 0;
    int inc = q->op == 11 || q->op == 12, sysf = q->op == 9 || q->op == 10, drv = q->op >= 13 && q->op <= 16;
    const char *dir = inc ? inc_dir : sysf ? sys_dir : drv ? drv_dir : sandbox_dir;
    if (!dir || q->op < 1 || q->op > 16) return -101;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (q->op == 5 || q->op == 16) return unlink(path) ? -2 : 0;
    if (q->op == 2 || q->op == 15) {
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
    if (q->op == 1 || q->op == 11 || q->op == 9 || q->op == 14) {
        if (q->buf_len == 0) r = -101;
        else if (fseek(f, (long)q->off, SEEK_SET) != 0) r = -101;
        else r = (long)fread((void *)q->buf_ptr, 1, q->buf_len, f);
    }
    fclose(f);
    return r;
}

/* Сеть Киры на сокетах хоста (для drvget: TLS работает внутри программы поверх этих вызовов). Адрес - число 0xAABBCCDD, как в k_net_*. */
static long net_dns(const char *name, uint64_t len) {
    char b[64]; struct in_addr a;
    if (len == 0 || len >= sizeof b) return -10;
    memcpy(b, name, len); b[len] = 0;
    return inet_pton(AF_INET, b, &a) == 1 ? (long)ntohl(a.s_addr) : -2;
}
static long net_connect(uint32_t ip, uint32_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port); sa.sin_addr.s_addr = htonl(ip);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa)) { if (fd >= 0) close(fd); return -2; }
    return fd;
}
static long net_recv(int fd, void *p, uint64_t n, uint32_t ms) {
    struct pollfd pf = { fd, POLLIN, 0 };
    int pr = poll(&pf, 1, ms ? (int)ms : 10000);
    if (pr <= 0) return -5;
    ssize_t r = recv(fd, p, (size_t)n, 0);
    return r < 0 ? -2 : (long)r;
}

static unsigned long c_arg(const greg_t *g) { return (unsigned long)g[REG_RDX]; }

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
    case K_NET_DNS: r = net_dns((const char *)a, b); break;
    case K_NET_CONNECT: r = net_connect((uint32_t)a, (uint32_t)b); break;
    case K_NET_SEND: r = send((int)a, (const void *)b, (size_t)c_arg(g), MSG_NOSIGNAL); break;
    case K_NET_RECV: r = net_recv((int)a, (void *)b, c_arg(g) & 0xffffffffu, (uint32_t)(c_arg(g) >> 32)); break;
    case K_NET_CLOSE: r = close((int)a); break;
    case K_CLOCK_NS: { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); r = (long)ts.tv_sec * 1000000000L + ts.tv_nsec; break; }
    case K_TIME: r = (long)time(NULL); break;
    case K_RANDOM: { ssize_t got = getrandom((void *)a, b > 256 ? 256 : b, 0); r = got < 0 ? -1 : 1; break; }
    case K_ARGS: { size_t n = strlen(prog_args); if (n + 1 > b) { r = -1; break; } memcpy((void *)a, prog_args, n + 1); r = (long)n; break; }
    case 7: r = -1; break;                    /* K_GFX_CREATE: у загрузчика нет экрана - окно не создаётся (drvman честно работает без окна) */
    case 55: r = -12; break;                  /* KDRV_SYS_PCILIST: ядра нет - список PCI недоступен (отказ, как без права CAP_PCILIST) */
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
        else if (argv[1][1] == 'y') sys_dir = argv[2];
        else if (argv[1][1] == 'd') drv_dir = argv[2];
        else if (argv[1][1] == 'a') prog_args = argv[2];
        else break;
        argv += 2; argc -= 2;
    }
    if (argc < 2) { fprintf(stderr, "usage: kloader [-s песочница] [-i заголовки] [-y /etc/ssl] [-d /drv] [-a args] prog.elf\n"); return 2; }
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
