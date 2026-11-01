/* Системные вызовы программ: номера и обработка общие для всех архитектур. Вход в ядро (SYSCALL на x86-64, SVC на
   ARM64) и регистры - в arch/<архитектура>/syscall_arch.c и ассемблере входа. */
#include "rng.h"
#include "rtc.h"
#include "crypto.h"
#include "syscall.h"
#include "net.h"
#include "audio.h"
#include "uart.h"
#include "vmm.h"
#include "proc.h"
#include "sched.h"
#include "timer.h"
#include "cpu.h"
#include "gfxsrv.h"
#include "fsrv.h"
#include "spinlock.h"
#include "userlog.h"
#include "kobj.h"
#include "futex.h"
#include "clock.h"
#include "heap.h"
#include "uaccess.h"

static spinlock_t ulog_lock = SPINLOCK_INIT;
static userlog_t ulog;

#define SYS_WRITE  1
#define SYS_EXIT   2
#define SYS_TICKS  3
#define SYS_SLEEP  4
#define SYS_YIELD  5
#define SYS_GETPID 6
#define SYS_GFX_CREATE  7   // (w,h) -> адрес пикселей 0x00RRGGBB в процессе или -1
#define SYS_GFX_PRESENT 8   // показать изменения
#define SYS_GFX_EVENT   9   // (ptr на 16 байт) -> 1 событие / 0 нет / -1 ошибка
#define SYS_HEAP       11   // (страниц) -> адрес нового куска кучи (нулевые RW-NX страницы) или отрицательный код
#define SYS_FS         10   // (ptr на fsrv_user_req_t) -> результат или отрицательный код; файлы только в песочнице программы
#define SYS_THREAD_CREATE 12 // (rip, вершина стека, аргумент) -> id потока
#define SYS_THREAD_EXIT   13 // (код): уходит поток; последний - процесс
#define SYS_GETTID        14
#define SYS_FUTEX_WAIT    15 // (адрес слова, ожидаемое, срок в 10 мс или ~0) -> 0 / KE_AGAIN / KE_TIMEOUT / KE_INTR
#define SYS_FUTEX_WAKE    16 // (адрес слова, сколько) -> разбужено
#define SYS_PORT_OPEN     17 // (имя, длина, флаги KOPEN_*) -> дескриптор
#define SYS_PORT_SEND     18 // (дескриптор, данные, длина до 1024) -> 0 / KE_FULL
#define SYS_PORT_RECV     19 // (дескриптор, буфер, ёмкость | срок_10мс << 32; срок 0xFFFFFFFF - без срока) -> длина | pid отправителя << 32
#define SYS_SHM_OPEN      20 // (имя, длина, страниц | флаги << 32) -> дескриптор
#define SYS_SHM_MAP       21 // (дескриптор) -> адрес
#define SYS_SHM_UNMAP     22 // (адрес)
#define SYS_CLOSE         23 // (дескриптор)
#define SYS_CLOCK_NS      24 // -> наносекунды монотонных часов
#define SYS_NET_DNS       25 // (имя, длина) -> адрес IPv4 или <0
#define SYS_NET_CONNECT   26 // (адрес, порт) -> номер соединения TCP
#define SYS_NET_SEND      27 // (h, буфер, длина) -> отправлено (подтверждено)
#define SYS_NET_RECV      28 // (h, буфер, длина | мс << 32) -> байт; 0 - собеседник закрыл
#define SYS_NET_CLOSE     29 // (h)
#define SYS_ARGS          30 // (буфер, ёмкость) -> длина строки аргументов запуска
#define SYS_NET_LISTEN    31 // (порт) -> номер слушателя (100+)
#define SYS_NET_ACCEPT    32 // (слушатель, мс; 0 - без срока) -> номер соединения
#define SYS_SND_OPEN      33 // () -> поток звука программы
#define SYS_SND_WRITE     34 // (поток, буфер, байт) -> принято байт (кадры 48 кГц, 16 бит, стерео)
#define SYS_SND_CLOSE     35 // (поток)
#define SYS_SND_VOLUME    42 // (поток, 0..100) - громкость своего потока
#define SYS_UDP_OPEN      38 // (порт или 0) -> сокет UDP
#define SYS_UDP_SEND      39 // (сокет | порт << 16 | адрес << 32, буфер, длина)
#define SYS_UDP_RECV      40 // (сокет, буфер, ёмкость | мс << 32) -> байт; буфер: адрес, порт, длина, данные
#define SYS_UDP_CLOSE     41 // (сокет)
#define SYS_TIME          37 // -> секунды UTC от 1970 (часы реального времени) или -1, если часов нет
#define SYS_RANDOM        36 // (буф, байт <= 256) -> 1 аппаратный источник, 0 - только время событий

#define SYS_MAX_WRITE 4096
#define SYS_MAX_SLEEP 1000

// Процесс завершился: его незаконченная строка выводится, место под строку освобождается.
void syscall_proc_gone(uint32_t pid) {
    uint64_t f = spin_lock_irqsave(&ulog_lock);
    userlog_flush(&ulog, pid, uart_print);
    spin_unlock_irqrestore(&ulog_lock, f);
}

// Вызывается с ВКЛЮЧЁННЫМИ прерываниями (их включает вход своей архитектуры: SYSCALL на x86-64, SVC на ARM64) на kernel-стеке
// потока: вызов можно вытеснить, общие данные ядра защищены собственными замками модулей.
static int copy_name(uint64_t ptr, uint64_t len, char out[KNAME_MAX + 1]) {
    if (len == 0 || len > KNAME_MAX || copy_from_user(out, ptr, len) != 0) return 0;
    out[len] = 0;
    return kname_ok(out);
}

static uint64_t ticks_from_10ms(uint64_t t) {
    if (t == 0xFFFFFFFFULL || t == ~0ULL) return ~0ULL;
    if (t > 100000) t = 100000;                                       /* не больше ~17 минут за один вызов */
    return t * TIMER_PER_10MS;
}

static int futex_key(uint64_t uaddr, uint64_t* key) {
    if ((uaddr & 3) || !vmm_user_range_ok(uaddr, 4, 0)) return 0;   /* адрес; само слово читает futex_wait безопасно */
    return vmm_translate(uaddr, key) == 0;                            /* физический адрес: общий для разделяемой памяти */
}

static uint64_t dispatch(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3);

uint64_t syscall_handler(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    proc_count_syscall();
    uint64_t r = dispatch(nr, a1, a2, a3);
    proc_return_check();                     /* другой поток завершил процесс: этот уходит, не возвращаясь в Ring 3 */
    return r;
}

static uint64_t dispatch(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    switch (nr) {
    case SYS_WRITE: {
        if (a2 == 0 || a2 > SYS_MAX_WRITE) return (uint64_t)-1;
        uint32_t pid = (uint32_t)proc_current_pid();
        for (uint64_t i = 0; i < a2; ) {                   // порциями: на 115200 бод 64 знака - около 6 мс без прерываний
            uint64_t n = a2 - i < 64 ? a2 - i : 64;
            char b[64];                                    // копия до замка: под спин-замком память программы не трогаем
            if (copy_from_user(b, a1 + i, n) != 0) return (uint64_t)-1;
            uint64_t f = spin_lock_irqsave(&ulog_lock);
            userlog_write(&ulog, pid, b, (size_t)n, uart_print);
            spin_unlock_irqrestore(&ulog_lock, f);
            i += n;
        }
        return a2;
    }
    case SYS_EXIT:
        proc_exit((int)(a1 & 0x7FFFFFFF));
    case SYS_TICKS:
        return timer_get_ticks() / TIMER_PER_10MS;       /* для программ тик по-прежнему 10 мс */
    case SYS_SLEEP:
        if (a1 > SYS_MAX_SLEEP) a1 = SYS_MAX_SLEEP;
        if (!proc_dying()) sched_sleep_ticks((uint32_t)a1 * TIMER_PER_10MS);   /* процесс завершается - не засыпать */
        return 0;
    case SYS_YIELD:
        sched_yield();
        return 0;
    case SYS_GETPID:
        return proc_current_pid();
    case SYS_GFX_CREATE: {
        proc_space_lock();                   /* отображение окна меняет адресное пространство, как и рост кучи */
        int64_t r = gfx_sys_create(proc_current_pid(), proc_current_name(), (uint32_t)a1, (uint32_t)a2);
        proc_space_unlock();
        return (uint64_t)r;
    }
    case SYS_GFX_PRESENT:
        return (uint64_t)gfx_sys_present(proc_current_pid());
    case SYS_GFX_EVENT:
        return (uint64_t)gfx_sys_event(proc_current_pid(), a1);
    case SYS_HEAP:
        return (uint64_t)proc_heap_grow(a1);
    case SYS_FS:
        return (uint64_t)fsrv_syscall(a1);
    case SYS_THREAD_CREATE:
        return (uint64_t)proc_thread_create(a1, a2, a3);
    case SYS_THREAD_EXIT:
        proc_thread_exit((int)(a1 & 0x7FFFFFFF));
    case SYS_GETTID:
        return sched_current()->id;
    case SYS_FUTEX_WAIT: {
        uint64_t key;
        if (!futex_key(a1, &key)) return (uint64_t)(int64_t)KE_ARG;
        return (uint64_t)(int64_t)futex_wait(key, a1, user_read32, (uint32_t)a2, ticks_from_10ms(a3), proc_dying);
    }
    case SYS_FUTEX_WAKE: {
        uint64_t key;
        if (!futex_key(a1, &key)) return (uint64_t)(int64_t)KE_ARG;
        return futex_wake(key, a2 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (unsigned)a2);
    }
    case SYS_PORT_OPEN: {
        char name[KNAME_MAX + 1];
        if (!copy_name(a1, a2, name)) return (uint64_t)(int64_t)KE_ARG;
        kobj_t* o = 0;
        int rc = port_open(name, (int)(a3 & 3), &o);
        if (rc) return (uint64_t)(int64_t)rc;
        return (uint64_t)proc_handle_add(o);
    }
    case SYS_PORT_SEND: {
        if (a3 > PORT_MSG_MAX) return (uint64_t)(int64_t)KE_ARG;
        uint8_t* buf = (uint8_t*)kmalloc(a3 ? a3 : 1);              /* копия: программа не изменит данные во время отправки */
        if (!buf) return (uint64_t)(int64_t)KE_NOMEM;
        if (a3 && copy_from_user(buf, a2, a3) != 0) { kfree(buf); return (uint64_t)(int64_t)KE_ARG; }
        kobj_t* o = proc_handle_get((int)a1, KOBJ_PORT);
        if (!o) { kfree(buf); return (uint64_t)(int64_t)KE_BADH; }
        int rc = port_send(o, proc_current_pid(), buf, (uint32_t)a3);
        kfree(buf);
        kobj_release(o);
        return (uint64_t)(int64_t)rc;
    }
    case SYS_PORT_RECV: {
        uint32_t cap = (uint32_t)a3, tmo = (uint32_t)(a3 >> 32);
        if (cap > PORT_MSG_MAX) cap = PORT_MSG_MAX;
        if (cap && !user_range(a2, cap, 1)) return (uint64_t)(int64_t)KE_ARG;
        kobj_t* o = proc_handle_get((int)a1, KOBJ_PORT);
        if (!o) return (uint64_t)(int64_t)KE_BADH;
        uint8_t* kb = (uint8_t*)kmalloc(cap ? cap : 1);
        if (!kb) { kobj_release(o); return (uint64_t)(int64_t)KE_NOMEM; }
        uint32_t from = 0;
        int64_t n = port_recv(o, kb, cap, ticks_from_10ms(tmo), &from, proc_dying);
        kobj_release(o);
        if (n >= 0) {
            uint32_t c = (uint64_t)n < cap ? (uint32_t)n : cap;
            if (c && copy_to_user(a2, kb, c) != 0) n = KE_ARG;           /* память программы исчезла: сообщение потеряно */
            else n = (int64_t)((uint64_t)(uint32_t)n | ((uint64_t)from << 32));
        }
        kfree(kb);
        return (uint64_t)n;
    }
    case SYS_SHM_OPEN: {
        char name[KNAME_MAX + 1];
        if (!copy_name(a1, a2, name)) return (uint64_t)(int64_t)KE_ARG;
        kobj_t* o = 0;
        int rc = shm_open(name, (uint32_t)a3, (int)((a3 >> 32) & 3), &o);
        if (rc) return (uint64_t)(int64_t)rc;
        return (uint64_t)proc_handle_add(o);
    }
    case SYS_SHM_MAP:
        return (uint64_t)proc_shm_map((int)a1);
    case SYS_SHM_UNMAP:
        return (uint64_t)proc_shm_unmap(a1);
    case SYS_CLOSE:
        return (uint64_t)proc_handle_close((int)a1);
    case SYS_CLOCK_NS:
        return clock_ns();
    case SYS_NET_DNS: case SYS_NET_CONNECT: case SYS_NET_SEND: case SYS_NET_RECV: case SYS_NET_CLOSE:
    case SYS_NET_LISTEN: case SYS_NET_ACCEPT:
        return (uint64_t)net_sys((unsigned)(nr - SYS_NET_DNS), a1, a2, a3);
    case SYS_SND_OPEN: case SYS_SND_WRITE: case SYS_SND_CLOSE:
        return (uint64_t)audio_sys((unsigned)(nr - SYS_SND_OPEN), a1, a2, a3);
    case SYS_SND_VOLUME:
        return (uint64_t)audio_sys(3, a1, a2, 0);
    case SYS_UDP_OPEN: case SYS_UDP_SEND: case SYS_UDP_RECV: case SYS_UDP_CLOSE:
        return (uint64_t)net_udp_sys((unsigned)(nr - SYS_UDP_OPEN), a1, a2, a3);
    case SYS_TIME: {
        rtc_time_t tm;
        return rtc_now(0, &tm) == 0 ? (uint64_t)rtc_to_epoch(&tm) : (uint64_t)-1;
    }
    case SYS_RANDOM: {
        uint8_t rb[256];
        if (a2 > sizeof rb) return (uint64_t)-2;
        int q = rng_get(rb, a2);
        int bad = a2 && copy_to_user(a1, rb, a2) != 0;
        crypto_wipe(rb, sizeof rb);
        return bad ? (uint64_t)-10 : (uint64_t)q;
    }
    case SYS_ARGS: {
        const char* s = proc_current_args();
        uint64_t len = 0; while (s[len]) len++;
        if (!a2 || len + 1 > a2) return (uint64_t)-10;
        return copy_to_user(a1, s, len + 1) ? (uint64_t)-10 : len;
    }
    default:
        return (uint64_t)-1;
    }
}
