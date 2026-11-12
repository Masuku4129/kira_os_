// Хост-тест объектов ядра: имена, дескрипторы, порты (очередь, переполнение, ожидание со сроком и пробуждение),
// разделяемая память (обнуление, общие страницы, освобождение последней ссылкой), futex (значение, срок, пробуждение).
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "kobj.h"
#include "futex.h"
#include "sched.h"
#include "spinlock.h"

void uart_print(const char* s) { (void)s; }
void uart_print_dec(uint64_t v) { (void)v; }
void uart_print_hex(uint64_t v) { (void)v; }
void* kmalloc(size_t n) { return malloc(n); }
void kfree(void* p) { free(p); }
uint64_t shm_room_kb(void) { return 64 * 1024; }
static int pages_live;
uint64_t pmm_alloc_page(void) { pages_live++; return (uint64_t)(uintptr_t)aligned_alloc(4096, 4096); }
int pmm_free_page(uint64_t pa) { pages_live--; free((void*)(uintptr_t)pa); return 0; }

static uint64_t fake_ticks;
uint64_t timer_get_ticks(void) { return fake_ticks; }
static thread_t me;
const thread_t* sched_current(void) { return &me; }
static void (*while_asleep)(void);          /* «другой поток», который действует, пока этот спит */
static uint64_t slept;
void sched_wait_and_unlock(spinlock_t* l, uint64_t ticks) {
    spin_unlock_raw(l);
    slept++;
    if (while_asleep) { void (*h)(void) = while_asleep; while_asleep = 0; h(); }
    else fake_ticks += ticks == ~0ULL ? 1000 : ticks;        /* никто не разбудил: срок вышел */
}
static int woken;
void sched_wake_waiter(thread_t* t) { if (t == &me) woken++; }

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static kobj_t* g_port;
static void send_hello(void) { port_send(g_port, 42, "hello", 5); }
static volatile uint32_t word = 7;
static uint64_t key = 0x1000;
static void wake_word(void) { CHECK(futex_wake(key, 5) == 1); }
static int dying;
static int stop_fn(void) { return dying; }

static int load_word(uint64_t w, uint32_t* v) { *v = *(volatile uint32_t*)(uintptr_t)w; return 0; }

int main(void) {
    CHECK(!kname_ok(0));
    CHECK(kname_ok("kira.test-1_a") && !kname_ok("") && !kname_ok("Bad") && !kname_ok("a/b") && !kname_ok("0123456789012345678901234567890123"));

    // дескрипторы
    htab_t h; memset(&h, 0, sizeof h);
    kobj_t a = { KOBJ_PORT, 1, "x" }, b = { KOBJ_SHM, 1, "y" };
    int ha = htab_add(&h, &a), hb = htab_add(&h, &b);
    CHECK(ha == 1 && hb == 2 && htab_get(&h, ha, KOBJ_PORT) == &a && htab_get(&h, ha, KOBJ_SHM) == 0 && htab_get(&h, 99, 0) == 0);
    CHECK(htab_take(&h, ha) == &a && htab_get(&h, ha, 0) == 0 && htab_add(&h, &a) == 1);
    for (int i = 0; i < 5000; i++) CHECK(htab_add(&h, &a) > 0);           /* таблица растёт сама */
    CHECK(htab_cap(&h) >= 5002 && htab_get(&h, 5002, KOBJ_PORT) == &a);
    htab_free(&h);

    // порты: создание, повторное открытие, EXCL, нет такого
    kobj_t *p1 = 0, *p2 = 0, *p3 = 0;
    CHECK(port_open("svc", 0, 0) == KE_ARG);
    CHECK(shm_open("area", 1, KOPEN_CREATE, 0) == KE_ARG);
    CHECK(port_queued(0) == 0);
    CHECK(port_open("svc", 0, &p1) == KE_NOENT);
    CHECK(port_open("svc", KOPEN_CREATE, &p1) == 0 && p1 && p1->refs == 1);
    CHECK(port_open("svc", KOPEN_CREATE | KOPEN_EXCL, &p2) == KE_EXIST);
    CHECK(port_open("svc", 0, &p2) == 0 && p2 == p1 && p1->refs == 2);
    CHECK(kobj_count() == 1);

    // очередь: порядок, отправитель, переполнение, длина больше ёмкости
    char buf[64]; uint32_t from = 0;
    CHECK(port_send(p1, 1, 0, 1) == KE_ARG);
    CHECK(port_recv(p1, 0, sizeof buf, 0, &from, 0) == KE_ARG);
    CHECK(port_recv(p1, buf, sizeof buf, 0, &from, 0) == KE_AGAIN);
    CHECK(port_send(p1, 5, "one", 3) == 0 && port_send(p1, 6, "two!", 4) == 0);
    CHECK(port_recv(p1, buf, sizeof buf, 0, &from, 0) == 3 && !memcmp(buf, "one", 3) && from == 5);
    CHECK(port_recv(p1, buf, 2, 0, &from, 0) == 4 && !memcmp(buf, "tw", 2) && from == 6);   /* длина - полная, копия - по ёмкости */
    for (int i = 0; i < PORT_QUEUE; i++) CHECK(port_send(p1, 1, "m", 1) == 0);
    CHECK(port_send(p1, 1, "m", 1) == KE_FULL);
    CHECK(port_send(p1, 1, buf, PORT_MSG_MAX + 1) == KE_ARG);
    while (port_recv(p1, buf, sizeof buf, 0, &from, 0) >= 0) { }

    // ожидание: срок выходит; сообщение приходит, пока поток спит; процесс завершается
    fake_ticks = 0;
    CHECK(port_recv(p1, buf, sizeof buf, 10, &from, 0) == KE_TIMEOUT && fake_ticks >= 10);
    g_port = p1; woken = 0; while_asleep = send_hello;
    CHECK(port_recv(p1, buf, sizeof buf, ~0ULL, &from, 0) == 5 && !memcmp(buf, "hello", 5) && from == 42 && woken == 1);
    dying = 1;
    CHECK(port_recv(p1, buf, sizeof buf, ~0ULL, &from, stop_fn) == KE_INTR);
    dying = 0;

    // освобождение: очередь с сообщениями уничтожается вместе с портом
    port_send(p1, 1, "left", 4);
    kobj_release(p2);
    CHECK(kobj_count() == 1);
    kobj_release(p1);
    CHECK(kobj_count() == 0 && port_open("svc", 0, &p3) == KE_NOENT);

    // разделяемая память
    kobj_t *s1 = 0, *s2 = 0;
    CHECK(shm_open("area", 0, KOPEN_CREATE, &s1) == KE_ARG);
    CHECK(shm_open("area", 100000, KOPEN_CREATE, &s1) == KE_NOMEM);      /* больше половины свободной памяти */
    CHECK(shm_open("area", 3, KOPEN_CREATE, &s1) == 0 && shm_pages(s1) == 3 && pages_live == 3);
    uint8_t* pg = (uint8_t*)(uintptr_t)shm_frame(s1, 2);
    int zero = 1; for (int i = 0; i < 4096; i++) if (pg[i]) zero = 0;
    CHECK(zero && shm_frame(s1, 3) == 0);
    pg[100] = 0x5A;
    CHECK(shm_open("area", 9, KOPEN_CREATE, &s2) == 0 && s2 == s1 && shm_pages(s2) == 3);   /* открыт существующий: размер прежний */
    CHECK(((uint8_t*)(uintptr_t)shm_frame(s2, 2))[100] == 0x5A);
    kobj_t* sp = 0; CHECK(port_open("area", 0, &sp) == KE_NOENT);                     /* у портов и памяти разные имена */
    kobj_release(s1); CHECK(pages_live == 3);
    kobj_release(s2); CHECK(pages_live == 0 && kobj_count() == 0);

    // futex
    CHECK(futex_wait(key, (uint64_t)(uintptr_t)&word, load_word, 8, ~0ULL, 0) == KE_AGAIN);                 /* значение уже другое: не спит */
    fake_ticks = 0;
    CHECK(futex_wait(key, (uint64_t)(uintptr_t)&word, load_word, 7, 20, 0) == KE_TIMEOUT && slept);
    woken = 0; while_asleep = wake_word;
    CHECK(futex_wait(key, (uint64_t)(uintptr_t)&word, load_word, 7, ~0ULL, 0) == 0 && woken == 1);
    CHECK(futex_wake(key, 1) == 0);                                          /* очередь пуста */
    dying = 1; CHECK(futex_wait(key, (uint64_t)(uintptr_t)&word, load_word, 7, ~0ULL, stop_fn) == KE_INTR); dying = 0;

    printf("%s\n", fails ? "FAILED" : "kobj: names, handles, ports (order, overflow, wait/timeout/wake, kill), shared memory, futex passed");
    return fails != 0;
}
