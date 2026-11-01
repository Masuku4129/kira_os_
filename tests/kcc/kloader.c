/* Хост-загрузчик ELF Киры для тестов kcc (Linux x86-64).
 * Образ проверяется настоящим elf_parse() ядра (окно пользователя, W^X, выравнивание, точка входа), сегменты отображаются
 * по их виртуальным адресам с правами R-X / R-- / RW-, затем управление передаётся в точку входа.
 * Программа Киры делает инструкцию syscall с номерами Киры; seccomp(SECCOMP_RET_TRAP) перехватывает именно syscall из
 * окна адресов 0x8000000000..0x8fffffffff (код загруженной программы) и обрабатывается таблицей k_* ниже. Остальные вызовы хоста не затрагиваются.
 * Печатает "<код выхода>" программы так же, как эталон: printf("%d\n", main()). */
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

enum { K_WRITE = 1, K_EXIT = 2, K_TICKS = 3, K_SLEEP = 4, K_YIELD = 5, K_GETPID = 6, K_HEAP = 11 };

static unsigned long heap_pages;

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
    if (argc < 2) { fprintf(stderr, "usage: kloader prog.elf\n"); return 2; }
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
