/* kcc libc-lite: kira.h - API программ Киры (x86-64 Ring 3) в объёме, который умеет kcc.
   Упрощённая версия userland/kira.h: без __atomic_* (потоки, мьютексы) и без файлового asm. Только системные вызовы.
   Права берёт ядро по имени программы: у собранной на Кире (runlocal) прав нет, вызовы к сети, файлам и окнам вернут отказ KE_PERM
   (-12; для файлов -100). Консоль, выход, часы, куча, аргументы доступны всегда. */
#ifndef KCC_KIRA_H
#define KCC_KIRA_H
#ifndef __x86_64__
#error "kira.h: kcc пока собирает программы только для x86-64 (AArch64 - этап M4)"
#endif
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
typedef unsigned long long u64; typedef long long i64;

/* rax = номер, rdi/rsi/rdx = аргументы; результат в rax (rdi/rsi/rdx ядро обнуляет). */
static i64 ksys(u64 n, u64 a, u64 b, u64 c) {
    i64 r;
    __asm__ volatile ("syscall" : "=a"(r), "+D"(a), "+S"(b), "+d"(c) : "a"(n) : "rcx", "r11", "r8", "r9", "r10", "memory");
    return r;
}

enum { K_WRITE = 1, K_EXIT = 2, K_TICKS = 3, K_SLEEP = 4, K_YIELD = 5, K_GETPID = 6, K_FS = 10, K_HEAP = 11,
       K_CLOCK_NS = 24, K_NET_DNS = 25, K_NET_CONNECT = 26, K_NET_SEND = 27, K_NET_RECV = 28, K_NET_CLOSE = 29,
       K_ARGS = 30, K_RANDOM = 36, K_TIME = 37 };
/* Ошибки ядра: -2 нет такого, -7 нет памяти, -10 неверные аргументы, -12 нет права (KE_PERM). */
enum { KE_NOENT = -2, KE_NOMEM = -7, KE_ARG = -10, KE_PERM = -12 };

static u64 k_strlen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
/* Консоль: строки идут в журнал ядра с пометкой процесса; строка выходит по знаку конца строки. */
static void k_write(const char *s, u64 n) { ksys(K_WRITE, (u64)s, n, 0); }
static void k_puts(const char *s) { k_write(s, k_strlen(s)); }
static void k_exit(int code) __attribute__((noreturn));
static void k_exit(int code) { for (;;) ksys(K_EXIT, (u64)code, 0, 0); }
static u64 k_ticks(void) { return (u64)ksys(K_TICKS, 0, 0, 0); }         /* тики по 10 мс от запуска системы */
static void k_sleep(u64 ticks) { ksys(K_SLEEP, ticks, 0, 0); }
static void k_yield(void) { ksys(K_YIELD, 0, 0, 0); }
static u64 k_getpid(void) { return (u64)ksys(K_GETPID, 0, 0, 0); }
static u64 k_clock_ns(void) { return (u64)ksys(K_CLOCK_NS, 0, 0, 0); }  /* монотонные наносекунды */
static i64 k_time(void) { return ksys(K_TIME, 0, 0, 0); }                /* секунды UTC от 1970; -1 - часов нет */
static i64 k_args(char *buf, u64 cap) { return ksys(K_ARGS, (u64)buf, cap, 0); }   /* строка аргументов запуска: длина или < 0 */
/* Куча: n страниц по 4 КБ нулевой памяти, куски идут подряд; 0 при отказе. */
static void *k_heap_grow(u64 pages) { i64 r = ksys(K_HEAP, pages, 0, 0); return r < 0 ? (void *)0 : (void *)r; }

/* Файлы песочницы /data/<имя программы> (право CAP_FS): короткие имена без '/', файл целиком. */
enum { KFS_READ = 1, KFS_WRITE = 2, KFS_STAT = 3, KFS_REMOVE = 5 };
struct kfs_req { u32 op, name_len; u64 name_ptr, buf_ptr, buf_len, off; };
static i64 k_fs(u32 op, const char *name, u64 name_len, void *buf, u64 len, u64 off) {
    struct kfs_req r;
    r.op = op; r.name_len = (u32)name_len; r.name_ptr = (u64)name; r.buf_ptr = (u64)buf; r.buf_len = len; r.off = off;
    return ksys(K_FS, (u64)&r, 0, 0);
}
static i64 k_file_read(const char *name, void *buf, u64 cap, u64 off) { return k_fs(KFS_READ, name, k_strlen(name), buf, cap, off); }
static i64 k_file_write(const char *name, const void *buf, u64 len) { return k_fs(KFS_WRITE, name, k_strlen(name), (void *)buf, len, 0); }
static i64 k_file_size(const char *name) { return k_fs(KFS_STAT, name, k_strlen(name), (void *)0, 0, 0); }
static i64 k_file_remove(const char *name) { return k_fs(KFS_REMOVE, name, k_strlen(name), (void *)0, 0, 0); }

/* Сеть TCP (право CAP_NET). Адрес IPv4: старший байт - первое число. */
static i64 k_net_dns(const char *name) { return ksys(K_NET_DNS, (u64)name, k_strlen(name), 0); }
static i64 k_net_connect(u32 ip, u32 port) { return ksys(K_NET_CONNECT, ip, port, 0); }
static i64 k_net_send(i64 h, const void *p, u64 n) { return ksys(K_NET_SEND, (u64)h, (u64)p, n); }
static i64 k_net_recv(i64 h, void *p, u32 n, u32 timeout_ms) { return ksys(K_NET_RECV, (u64)h, (u64)p, (u64)n | ((u64)timeout_ms << 32)); }
static i64 k_net_close(i64 h) { return ksys(K_NET_CLOSE, (u64)h, 0, 0); }
#endif
