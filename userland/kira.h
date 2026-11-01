#ifndef KIRA_USER_H
#define KIRA_USER_H
/* API программ для Kira OS (x86-64 Ring 3 и ARM64 EL0). Без libc: только системные вызовы.
   x86-64: rax = номер, rdi/rsi/rdx = аргументы, результат в rax. ARM64: x8 = номер, x0..x2, результат в x0. */
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;
typedef unsigned long long u64; typedef long long i64;

#if defined(__aarch64__)
/* ARM64: svc #0, номер в x8, аргументы x0..x2, результат в x0; ядро возвращает остальные регистры нетронутыми. */
static inline i64 ksys(u64 n, u64 a, u64 b, u64 c) {
    register u64 x8 __asm__("x8") = n;
    register u64 x0 __asm__("x0") = a;
    register u64 x1 __asm__("x1") = b;
    register u64 x2 __asm__("x2") = c;
    __asm__ volatile ("svc #0" : "+r"(x0), "+r"(x1), "+r"(x2) : "r"(x8) : "memory");
    return (i64)x0;
}
#else
static inline i64 ksys(u64 n, u64 a, u64 b, u64 c) {
    i64 r;
    /* Ядро обнуляет rdi/rsi/rdx/r8-r10 при возврате (не отдаёт процессу свои значения). */
    __asm__ volatile ("syscall" : "=a"(r), "+D"(a), "+S"(b), "+d"(c) : "a"(n) : "rcx", "r11", "r8", "r9", "r10", "memory");
    return r;
}
#endif

enum { K_FS = 10, K_HEAP = 11 };
enum { KFS_READ = 1, KFS_WRITE = 2, KFS_STAT = 3, KFS_LIST = 4, KFS_REMOVE = 5, KFS_GRANT_READ = 6, KFS_GRANT_STAT = 7, KFS_GRANT_NAME = 8, KFS_SYS_READ = 9, KFS_SYS_STAT = 10 };
typedef struct { u32 op, name_len; u64 name_ptr, buf_ptr, buf_len, off; } kfs_req_t;
enum { K_WRITE = 1, K_EXIT = 2, K_TICKS = 3, K_SLEEP = 4, K_YIELD = 5, K_GETPID = 6,
       K_GFX_CREATE = 7, K_GFX_PRESENT = 8, K_GFX_EVENT = 9 };

typedef struct { u32 type, a, b, c; } kevent_t;
/* type: 1 клавиша вниз (a=код, b=символ, c=модификаторы), 2 клавиша вверх,
         3 мышь двигается (a=x, b=y в окне, c=кнопки), 4 кнопки изменились, 5 окно просят закрыть */
enum { KEV_KEY_DOWN = 1, KEV_KEY_UP = 2, KEV_MOUSE_MOVE = 3, KEV_MOUSE_BUTTON = 4, KEV_CLOSE = 5 };

static inline void k_write(const char* s, u64 n) { ksys(K_WRITE, (u64)s, n, 0); }
static inline void k_sleep(u64 ticks) { ksys(K_SLEEP, ticks, 0, 0); }
static inline u64 k_ticks(void) { return (u64)ksys(K_TICKS, 0, 0, 0); }
static inline u64 k_getpid(void) { return (u64)ksys(K_GETPID, 0, 0, 0); }
static inline __attribute__((noreturn)) void k_exit(int code) { for (;;) ksys(K_EXIT, (u64)code, 0, 0); }
/* Окно w x h пикселей. Возвращает указатель на пиксели 0x00RRGGBB (шаг = w) или 0. */
static inline u32* k_gfx_create(u32 w, u32 h) { i64 r = ksys(K_GFX_CREATE, w, h, 0); return r < 0 ? (u32*)0 : (u32*)(u64)r; }
static inline void k_gfx_present(void) { ksys(K_GFX_PRESENT, 0, 0, 0); }
static inline int k_gfx_event(kevent_t* e) { return (int)ksys(K_GFX_EVENT, (u64)e, 0, 0); }

/* Куча: n страниц по 4 КБ нулевой памяти (RW). Возвращает адрес НОВОГО куска (куски идут подряд) или 0 при отказе
   (предел 4 МБ на программу, нет памяти). Освобождается при завершении программы. */
static inline void* k_heap_grow(u64 pages) { i64 r = ksys(K_HEAP, pages, 0, 0); return r < 0 ? (void*)0 : (void*)(u64)r; }

/* Файлы программы: только её песочница /data/<имя программы>, имена короткие (без '/'): буквы, цифры, '.', '-', '_'.
   Результат - число байт / размер / 0 либо отрицательный код (-100 запрещено, -101 неверные аргументы, -102 слишком много файлов). Файл до 64 КБ. */
static inline i64 k_fs(u32 op, const char* name, u64 name_len, void* buf, u64 len, u64 off) {
    kfs_req_t r = { op, (u32)name_len, (u64)name, (u64)buf, len, off };
    return ksys(K_FS, (u64)&r, 0, 0);
}
static inline u64 k_strlen(const char* s) { u64 n = 0; while (s[n]) n++; return n; }
static inline i64 k_file_read(const char* name, void* buf, u64 cap, u64 off) { return k_fs(KFS_READ, name, k_strlen(name), buf, cap, off); }
static inline i64 k_file_write(const char* name, const void* buf, u64 len) { return k_fs(KFS_WRITE, name, k_strlen(name), (void*)buf, len, 0); }
static inline i64 k_file_size(const char* name) { return k_fs(KFS_STAT, name, k_strlen(name), (void*)0, 0, 0); }
static inline i64 k_file_remove(const char* name) { return k_fs(KFS_REMOVE, name, k_strlen(name), (void*)0, 0, 0); }
static inline i64 k_file_list(char* buf, u64 cap) { return k_fs(KFS_LIST, "", 0, buf, cap, 0); }
/* Файл, выданный при запуске (открыт из Проводника): размер, путь, чтение кусками (вне песочницы, только он). */
static inline i64 k_grant_size(void) { return k_fs(KFS_GRANT_STAT, "", 0, (void*)0, 0, 0); }
static inline i64 k_grant_name(char* buf, u64 cap) { return k_fs(KFS_GRANT_NAME, "", 0, buf, cap, 0); }
/* Общие файлы системы (/etc/ssl/<имя>), только чтение: корневые сертификаты. */
static inline i64 k_sys_read(const char* name, void* buf, u64 cap, u64 off) { return k_fs(KFS_SYS_READ, name, k_strlen(name), buf, cap, off); }
static inline i64 k_sys_stat(const char* name) { return k_fs(KFS_SYS_STAT, name, k_strlen(name), 0, 0, 0); }
static inline i64 k_grant_read(void* buf, u64 len, u64 off) { return k_fs(KFS_GRANT_READ, "", 0, buf, len, off); }

/* ---- Потоки, futex, порты, разделяемая память, часы ---- */
enum { K_THREAD_CREATE = 12, K_THREAD_EXIT = 13, K_GETTID = 14, K_FUTEX_WAIT = 15, K_FUTEX_WAKE = 16,
       K_PORT_OPEN = 17, K_PORT_SEND = 18, K_PORT_RECV = 19, K_SHM_OPEN = 20, K_SHM_MAP = 21, K_SHM_UNMAP = 22,
       K_CLOSE = 23, K_CLOCK_NS = 24 };
/* Ошибки: -2 нет такого, -3 уже есть, -4 очередь полна, -5 срок вышел, -6 значение другое / нет сообщения,
   -7 нет памяти, -8 неверный дескриптор, -9 предел, -10 неверные аргументы, -11 процесс завершается. */
enum { KE_NOENT = -2, KE_EXIST = -3, KE_FULL = -4, KE_TIMEOUT = -5, KE_AGAIN = -6, KE_NOMEM = -7, KE_BADH = -8,
       KE_LIMIT = -9, KE_ARG = -10, KE_INTR = -11 };
enum { KOPEN_CREATE = 1, KOPEN_EXCL = 2 };
#define K_FOREVER 0xFFFFFFFFull

static inline u64 k_gettid(void) { return (u64)ksys(K_GETTID, 0, 0, 0); }
static inline u64 k_clock_ns(void) { return (u64)ksys(K_CLOCK_NS, 0, 0, 0); }
static inline __attribute__((noreturn)) void k_thread_exit(int code) { for (;;) ksys(K_THREAD_EXIT, (u64)code, 0, 0); }
#if defined(__aarch64__)
/* ARM64: поток начинается в трамплине: функция лежит в слоте на вершине стека, аргумент - в x0 (его ставит ядро);
   возврат из функции завершает поток. */
__asm__(".text\n.weak k_thread_tramp\nk_thread_tramp:\n ldr x9, [sp]\n blr x9\n mov x0, #0\n mov x8, #13\n svc #0\n1: b 1b\n");
void k_thread_tramp(void);
static inline i64 k_thread_create(void (*fn)(u64), void* stack, u64 size, u64 arg) {
    u64 top = ((u64)stack + size) & ~15ull;
    ((u64*)top)[-2] = (u64)fn;                   /* ядро ставит sp = переданной вершине (кратна 16) */
    return ksys(K_THREAD_CREATE, (u64)k_thread_tramp, top - 16, arg);
}
#else
static void __attribute__((noreturn, used)) k_thread_return(void) { k_thread_exit(0); }
/* Новый поток: fn(arg) на стеке [stack, stack+size) (память программы, например из k_heap_grow).
   Возврат из fn завершает поток. Результат - id потока или отрицательный код. */
static inline i64 k_thread_create(void (*fn)(u64), void* stack, u64 size, u64 arg) {
    u64 top = ((u64)stack + size) & ~15ull;
    ((u64*)top)[-1] = (u64)k_thread_return;      /* куда вернётся fn: ядро ставит rsp = top - 8 */
    return ksys(K_THREAD_CREATE, (u64)fn, top, arg);
}
#endif

/* futex: спать, пока *w == expected (срок в 10 мс, K_FOREVER - без срока); разбудить n ждущих на w. */
static inline i64 k_futex_wait(volatile u32* w, u32 expected, u64 timeout) { return ksys(K_FUTEX_WAIT, (u64)w, expected, timeout); }
static inline i64 k_futex_wake(volatile u32* w, u32 n) { return ksys(K_FUTEX_WAKE, (u64)w, n, 0); }
/* Мьютекс на futex: 0 свободен, 1 занят, 2 занят и есть ждущие. */
static inline void k_mutex_lock(volatile u32* m) {
    u32 c = 0;
    if (__atomic_compare_exchange_n(m, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    if (c != 2) c = __atomic_exchange_n(m, 2, __ATOMIC_ACQUIRE);
    while (c != 0) { k_futex_wait(m, 2, K_FOREVER); c = __atomic_exchange_n(m, 2, __ATOMIC_ACQUIRE); }
}
static inline void k_mutex_unlock(volatile u32* m) {
    if (__atomic_fetch_sub(m, 1, __ATOMIC_RELEASE) != 1) { __atomic_store_n(m, 0, __ATOMIC_RELEASE); k_futex_wake(m, 1); }
}
/* Порт - именованная очередь сообщений до 1024 байт. recv возвращает длину (младшие 32 бита) и pid отправителя
   (старшие) или отрицательный код. */
static inline i64 k_port_open(const char* name, u32 flags) { return ksys(K_PORT_OPEN, (u64)name, k_strlen(name), flags); }
static inline i64 k_port_send(i64 h, const void* data, u64 len) { return ksys(K_PORT_SEND, (u64)h, (u64)data, len); }
static inline i64 k_port_recv(i64 h, void* buf, u32 cap, u32 timeout) { return ksys(K_PORT_RECV, (u64)h, (u64)buf, (u64)cap | ((u64)timeout << 32)); }
/* Разделяемая память: страницы обнулены при создании; отображение - адрес в программе. */
static inline i64 k_shm_open(const char* name, u32 pages, u32 flags) { return ksys(K_SHM_OPEN, (u64)name, k_strlen(name), (u64)pages | ((u64)flags << 32)); }
static inline void* k_shm_map(i64 h) { i64 r = ksys(K_SHM_MAP, (u64)h, 0, 0); return r < 0 ? (void*)0 : (void*)(u64)r; }
static inline i64 k_shm_unmap(void* va) { return ksys(K_SHM_UNMAP, (u64)va, 0, 0); }
static inline i64 k_close(i64 h) { return ksys(K_CLOSE, (u64)h, 0, 0); }
/* ---- Сеть (TCP ядра) и аргументы запуска ---- */
enum { K_NET_DNS = 25, K_NET_CONNECT = 26, K_NET_SEND = 27, K_NET_RECV = 28, K_NET_CLOSE = 29, K_ARGS = 30 };
/* Адрес по имени (или строка «a.b.c.d»): >= 0 - адрес IPv4 (старший байт - первое число), < 0 - ошибка. */
static inline i64 k_net_dns(const char* name) { return ksys(K_NET_DNS, (u64)name, k_strlen(name), 0); }
static inline i64 k_net_connect(u32 ip, u32 port) { return ksys(K_NET_CONNECT, ip, port, 0); }   /* номер соединения */
static inline i64 k_net_send(i64 h, const void* p, u64 n) { return ksys(K_NET_SEND, (u64)h, (u64)p, n); }
/* Байт принято; 0 - собеседник закрыл; < 0 - сброс или срок (timeout_ms, 0 - 10 с). */
static inline i64 k_net_recv(i64 h, void* p, u32 n, u32 timeout_ms) { return ksys(K_NET_RECV, (u64)h, (u64)p, (u64)n | ((u64)timeout_ms << 32)); }
static inline i64 k_net_close(i64 h) { return ksys(K_NET_CLOSE, (u64)h, 0, 0); }   /* соединение или слушатель */
enum { K_NET_LISTEN = 31, K_NET_ACCEPT = 32, K_SND_OPEN = 33, K_SND_WRITE = 34, K_SND_CLOSE = 35, K_RANDOM = 36, K_TIME = 37 };
/* Звук: поток программы (кадры 48 кГц, 16 бит, стерео - 4 байта); write принимает, сколько влезло в очередь (1 с). */
static inline i64 k_snd_open(void) { return ksys(K_SND_OPEN, 0, 0, 0); }
static inline i64 k_snd_write(i64 h, const void* p, u64 bytes) { return ksys(K_SND_WRITE, (u64)h, (u64)p, bytes); }
static inline i64 k_snd_close(i64 h) { return ksys(K_SND_CLOSE, (u64)h, 0, 0); }
/* случайные байты ядра (до 256): 1 - засеяно аппаратным источником, 0 - только временем событий, <0 - ошибка */
static inline i64 k_random(void* buf, u64 n) { return ksys(K_RANDOM, (u64)buf, n, 0); }
static inline i64 k_time(void) { return ksys(K_TIME, 0, 0, 0); }      /* секунды UTC от 1970; -1 - часов нет */
static inline i64 k_net_listen(u32 port) { return ksys(K_NET_LISTEN, port, 0, 0); }             /* слушатель (100+) */
static inline i64 k_net_accept(i64 l, u32 timeout_ms) { return ksys(K_NET_ACCEPT, (u64)l, timeout_ms, 0); }   /* 0 мс - ждать без срока */
/* Строка аргументов запуска (run имя аргументы): длина или < 0. */
static inline i64 k_args(char* buf, u64 cap) { return ksys(K_ARGS, (u64)buf, cap, 0); }

/* Точка входа: выравнивает стек, вызывает umain, затем exit. */
#if defined(__aarch64__)
#define KIRA_ENTRY() \
    int umain(void); \
    __asm__(".section .text.start\n.global _start\n_start:\n mov x9, sp\n and x9, x9, #-16\n mov sp, x9\n mov x29, #0\n bl umain\n mov x0, #0\n mov x8, #2\n svc #0\n1: b 1b\n.text\n");
#else
#define KIRA_ENTRY() \
    int umain(void); \
    __asm__(".section .text.start\n.global _start\n_start:\n and $-16,%rsp\n call umain\n mov $2,%eax\n xor %edi,%edi\n syscall\n1: jmp 1b\n.text\n");
#endif
#endif
