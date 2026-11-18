// Хост-тест объектов ядра: имена, дескрипторы, порты (очередь, переполнение, ожидание со сроком и пробуждение),
// разделяемая память (обнуление, общие страницы, освобождение последней ссылкой), futex (значение, срок, пробуждение).
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "kobj.h"
#include "caps.h"
#include "futex.h"
#include "sched.h"
#include "spinlock.h"

void uart_print(const char* s) { (void)s; }
void uart_print_dec(uint64_t v) { (void)v; }
void uart_print_hex(uint64_t v) { (void)v; }
void* kmalloc(size_t n) { return malloc(n); }
void kfree(void* p) { free(p); }
static uint64_t shm_room_limit_kb = 64 * 1024;
uint64_t shm_room_kb(void) { return shm_room_limit_kb; }
static int pages_live;
static int fail_pmm_after = -1;
static void (*on_pmm_alloc)(void);
uint64_t pmm_alloc_page(void) {
    if (fail_pmm_after == 0) return 0;
    if (fail_pmm_after > 0) fail_pmm_after--;
    uint64_t pa = (uint64_t)(uintptr_t)aligned_alloc(4096, 4096);
    if (pa) pages_live++;
    if (on_pmm_alloc) { void (*fn)(void) = on_pmm_alloc; on_pmm_alloc = 0; fn(); }
    return pa;
}
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

static int racing_shm_result;
static kobj_t* racing_shm_object;
static void racing_shm_create(void) {
    racing_shm_result = shm_open_as("race.area", 1, KOPEN_CREATE, 902, &racing_shm_object);
}

static int racing_budget_result;
static kobj_t* racing_budget_object;
static void racing_budget_create(void) {
    racing_budget_result = shm_open_as("budget.other", 1, KOPEN_CREATE, 903, &racing_budget_object);
}

int main(void) {
    CHECK(kname_ok("kira.test-1_a") && !kname_ok(0) && !kname_ok("") && !kname_ok("Bad") && !kname_ok("a/b") && !kname_ok("0123456789012345678901234567890123"));

    // A concurrent creator must not allocate a second full SHM region for the same name.
    kobj_t* race_owner = 0;
    racing_shm_result = 0;
    racing_shm_object = 0;
    on_pmm_alloc = racing_shm_create;
    CHECK(shm_open_as("race.area", 1, KOPEN_CREATE, 901, &race_owner) == 0 && race_owner);
    CHECK(racing_shm_result == KE_AGAIN && racing_shm_object == 0);
    CHECK(pages_live == 1 && kobj_count() == 1);
    kobj_release(race_owner);
    CHECK(pages_live == 0 && kobj_count() == 0);

    // Concurrent SHM creators with different names must share the in-flight memory budget.
    shm_room_limit_kb = 4;
    racing_budget_result = 0; racing_budget_object = 0;
    on_pmm_alloc = racing_budget_create;
    kobj_t* budget_owner = 0;
    CHECK(shm_open_as("budget.first", 1, KOPEN_CREATE, 904, &budget_owner) == 0 && budget_owner);
    CHECK(racing_budget_result == KE_NOMEM && racing_budget_object == 0);
    CHECK(pages_live == 1 && kobj_count() == 1);
    kobj_release(budget_owner);
    CHECK(pages_live == 0 && kobj_count() == 0);
    shm_room_limit_kb = 64 * 1024;

    // Partial physical allocation failure must unwind frames and the pending budget reservation.
    fail_pmm_after = 1;
    kobj_t* partial = 0;
    CHECK(shm_open_as("partial.fail", 3, KOPEN_CREATE, 905, &partial) == KE_NOMEM && partial == 0);
    fail_pmm_after = -1;
    CHECK(pages_live == 0 && kobj_count() == 0);
    kobj_t* after_failure = 0;
    CHECK(shm_open_as("after.failure", 1, KOPEN_CREATE, 906, &after_failure) == 0 && after_failure);
    kobj_release(after_failure);
    CHECK(pages_live == 0 && kobj_count() == 0);

    // Приватные IPC-объекты доступны создателю; публикация задаётся только при создании.
    kobj_t *priv1 = 0, *priv2 = 0, *pub1 = 0, *pub2 = 0;
    CHECK(port_open_as("private.port", KOPEN_PUBLIC, 100, &priv1) == KE_ARG);
    CHECK(port_open_as("private.port", KOPEN_CREATE, 100, &priv1) == 0 && priv1);
    CHECK(port_open_as("private.port", 0, 200, &priv2) == KE_PERM);
    CHECK(port_open_as("private.port", KOPEN_CREATE | KOPEN_PUBLIC, 200, &priv2) == KE_PERM);
    CHECK(port_open_as("private.port", 0, 100, &priv2) == 0 && priv2 == priv1);
    kobj_release(priv2); kobj_release(priv1);
    CHECK(port_open_as("public.port", KOPEN_CREATE | KOPEN_PUBLIC, 101, &pub1) == 0 && pub1);
    CHECK(port_open_as("public.port", 0, 202, &pub2) == 0 && pub2 == pub1);
    kobj_release(pub2); kobj_release(pub1);

    kobj_t *priv_shm1 = 0, *priv_shm2 = 0, *pub_shm1 = 0, *pub_shm2 = 0;
    CHECK(shm_open_as("private.area", 1, KOPEN_CREATE, 301, &priv_shm1) == 0 && priv_shm1);
    CHECK(shm_open_as("private.area", 1, 0, 302, &priv_shm2) == KE_PERM);
    CHECK(shm_open_as("private.area", 1, 0, 301, &priv_shm2) == 0 && priv_shm2 == priv_shm1);
    kobj_release(priv_shm2); kobj_release(priv_shm1);
    CHECK(shm_open_as("public.area", 1, KOPEN_CREATE | KOPEN_PUBLIC, 401, &pub_shm1) == 0 && pub_shm1);
    CHECK(shm_open_as("public.area", 0, 0, 402, &pub_shm2) == 0 && pub_shm2 == pub_shm1);
    kobj_release(pub_shm2); kobj_release(pub_shm1);
    CHECK(kobj_count() == 0 && pages_live == 0);

    // Reference counts must never wrap and make a live object appear unreferenced.
    kobj_t* ref_limit = 0;
    CHECK(port_open("ref.limit", KOPEN_CREATE, &ref_limit) == 0 && ref_limit);
    if (ref_limit) {
        ref_limit->refs = UINT32_MAX;
        CHECK(kobj_retain(ref_limit) == KE_LIMIT);
        kobj_t* denied = 0;
        CHECK(port_open("ref.limit", 0, &denied) == KE_LIMIT && denied == 0);
        ref_limit->refs = 1;
        kobj_release(ref_limit);
    }

    // Null inputs are rejected safely by descriptor helpers; null object refs are no-ops.
    CHECK(htab_add(0, 0) == KE_ARG && htab_cap(0) == 0);
    CHECK(htab_get(0, 1, 0) == 0 && htab_take(0, 1) == 0);
    htab_free(0); CHECK(kobj_retain(0) == KE_ARG); kobj_release(0);
    kobj_t zero_refs = { KOBJ_PORT, 0, "zero" };
    CHECK(kobj_retain(&zero_refs) == KE_LIMIT);

    // дескрипторы
    htab_t h; memset(&h, 0, sizeof h);
    CHECK(htab_add(&h, 0) == KE_ARG);
    kobj_t a = { KOBJ_PORT, 1, "x" }, b = { KOBJ_SHM, 1, "y" };
    int ha = htab_add(&h, &a), hb = htab_add(&h, &b);
    CHECK(ha == 1 && hb == 2 && htab_get(&h, ha, KOBJ_PORT) == &a && htab_get(&h, ha, KOBJ_SHM) == 0 && htab_get(&h, 99, 0) == 0);
    CHECK(htab_take(&h, ha) == &a && htab_get(&h, ha, 0) == 0 && htab_add(&h, &a) == 1);
    for (int i = 0; i < 5000; i++) CHECK(htab_add(&h, &a) > 0);           /* таблица растёт сама */
    CHECK(htab_cap(&h) >= 5002 && htab_get(&h, 5002, KOBJ_PORT) == &a);
    htab_free(&h);

    // порты: создание, повторное открытие, EXCL, нет такого
    kobj_t *p1 = 0, *p2 = 0, *p3 = 0;
    CHECK(port_open("svc", 0, &p1) == KE_NOENT);
    CHECK(port_open("svc", KOPEN_CREATE, &p1) == 0 && p1 && p1->refs == 1);
    CHECK(port_open("svc", KOPEN_CREATE | KOPEN_EXCL, &p2) == KE_EXIST);
    CHECK(port_open("svc", 0, &p2) == 0 && p2 == p1 && p1->refs == 2);
    CHECK(kobj_count() == 1);

    // очередь: порядок, отправитель, переполнение, длина больше ёмкости
    char buf[64]; uint32_t from = 0;
    CHECK(port_recv(p1, buf, sizeof buf, 0, &from, 0) == KE_AGAIN);
    CHECK(port_send(p1, 1, 0, 1) == KE_ARG);
    CHECK(port_recv(p1, 0, 1, 0, &from, 0) == KE_ARG);
    CHECK(port_send(p1, 5, "one", 3) == 0 && port_send(p1, 6, "two!", 4) == 0);
    CHECK(port_queued(p1) == 2 && kobj_messages_total() >= 2);
    CHECK(port_recv(p1, buf, sizeof buf, 0, &from, 0) == 3 && !memcmp(buf, "one", 3) && from == 5);
    CHECK(port_queued(p1) == 1);
    CHECK(port_recv(p1, buf, 2, 0, &from, 0) == 4 && !memcmp(buf, "tw", 2) && from == 6);
    CHECK(port_queued(p1) == 0);   /* длина - полная, копия - по ёмкости */
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

    /* домены приложений: общий объект открывается только доменами, до которых дотягивается домен создателя */
    { kobj_t *o = 0, *r = 0;
    CHECK(port_open_in("dom.pub", KOPEN_CREATE | KOPEN_PUBLIC, 10, DOM_MEDIA, &o) == 0 && o);
    CHECK(port_open_in("dom.pub", 0, 11, DOM_MEDIA, &r) == 0 && r == o);
    CHECK(port_open_in("dom.pub", 0, 12, DOM_SYSTEM, &r) == 0 && r == o);
    CHECK(port_open_in("dom.pub", 0, 13, DOM_NET, &r) == KE_PERM);
    CHECK(port_open_in("dom.pub", 0, 14, DOM_TOOLS, &r) == KE_PERM);
    CHECK(port_open_in("dom.pub", 0, 15, DOM_UNTRUSTED, &r) == KE_PERM);
    CHECK(port_open_in("dom.pub", KOPEN_CREATE | KOPEN_EXCL, 11, DOM_MEDIA, &r) == KE_EXIST);
    /* Приватный объект по-прежнему только создателю, даже внутри домена и даже для system. */
    CHECK(port_open_in("dom.priv", KOPEN_CREATE, 10, DOM_MEDIA, &o) == 0);
    CHECK(port_open_in("dom.priv", 0, 11, DOM_MEDIA, &r) == KE_PERM && port_open_in("dom.priv", 0, 12, DOM_SYSTEM, &r) == KE_PERM);
    /* Память: то же правило. */
    CHECK(shm_open_in("dom.shm", 1, KOPEN_CREATE | KOPEN_PUBLIC, 20, DOM_NET, &o) == 0 && o);
    CHECK(shm_open_in("dom.shm", 1, 0, 21, DOM_NET, &r) == 0 && shm_open_in("dom.shm", 1, 0, 22, DOM_MEDIA, &r) == KE_PERM);
    }

    /* вложения в сообщения портов: передача объекта, права доменов, правило глубины 1 */
    { kobj_t *srv = 0, *rep = 0, *shm = 0, *got = 0, *other = 0; char buf[16]; uint32_t from = 0;
    unsigned objs0 = kobj_count(); uint64_t live0 = pages_live;
    CHECK(port_open_in("att.srv", KOPEN_CREATE | KOPEN_PUBLIC, 30, DOM_SYSTEM, &srv) == 0);
    CHECK(port_open_in("att.rep", KOPEN_CREATE, 31, DOM_MEDIA, &rep) == 0);              /* приватный порт клиента: сервер откроет его только из вложения */
    CHECK(shm_open_in("att.shm", 1, KOPEN_CREATE, 31, DOM_MEDIA, &shm) == 0);
    kobj_retain(rep);
    CHECK(port_send_att(srv, 31, "hi", 2, rep) == 0 && port_queued(srv) == 1);
    CHECK(port_recv_att(srv, buf, sizeof buf, 0, &from, 0, &got) == 2 && from == 31 && got == rep);   /* ссылка у получателя */
    kobj_release(got);
    kobj_retain(shm);
    CHECK(port_send_att(rep, 30, "d", 1, shm) == 0);                                      /* ответ с памятью в порт из вложения */
    CHECK(port_recv_att(rep, buf, sizeof buf, 0, &from, 0, &got) == 1 && got == shm);
    kobj_release(got);
    /* без att_out вложение отбрасывается, ссылки не текут */
    kobj_retain(shm);
    CHECK(port_send_att(rep, 30, "d", 1, shm) == 0 && port_recv(rep, buf, sizeof buf, 0, &from, 0) == 1);
    /* домен: порт SYSTEM принимает вложения из любого домена, до которого он дотягивается; медийный порт не принимает вложение домена NET */
    CHECK(port_open_in("att.net", KOPEN_CREATE | KOPEN_PUBLIC, 40, DOM_NET, &other) == 0);
    kobj_retain(other); CHECK(port_send_att(rep, 31, "x", 1, other) == KE_PERM); kobj_release(other);
    kobj_release(other);
    /* глубина 1: порт с вложением нельзя вложить; вложенному порту вложения нельзя; самого в себя нельзя */
    kobj_retain(shm); CHECK(port_send_att(rep, 30, "d", 1, shm) == 0);                    /* в rep лежит вложение */
    kobj_retain(rep); CHECK(port_send_att(srv, 31, "r", 1, rep) == KE_PERM); kobj_release(rep);   /* rep несёт вложение - вкладываться не может */
    CHECK(port_recv(rep, buf, sizeof buf, 0, &from, 0) == 1);                             /* вложение отброшено */
    kobj_retain(rep); CHECK(port_send_att(srv, 31, "r", 1, rep) == 0);                    /* теперь можно: rep в пути */
    kobj_retain(shm); CHECK(port_send_att(rep, 30, "d", 1, shm) == KE_PERM); kobj_release(shm);   /* вложенному порту вложения нельзя */
    CHECK(port_send_att(rep, 30, "plain", 5, 0) == 0);                                    /* обычные сообщения - можно */
    kobj_retain(srv); CHECK(port_send_att(srv, 30, "s", 1, srv) == KE_ARG); kobj_release(srv);
    CHECK(port_recv(srv, buf, sizeof buf, 0, &from, 0) == 1 && port_queued(srv) == 0);          /* "r" с вложением rep (без att_out - отброшено) */
    kobj_release(srv);
    while (port_recv(rep, buf, sizeof buf, 0, &from, 0) >= 0) {}
    kobj_release(rep); kobj_release(shm);
    CHECK(kobj_count() == objs0 + 0 && pages_live == live0);                               /* всё освобождено, включая вложения в очередях при уничтожении */
    /* уничтожение порта с вложениями в очереди отпускает вложения */
    CHECK(port_open_in("att.q", KOPEN_CREATE, 50, DOM_SYSTEM, &srv) == 0 && shm_open_in("att.q.shm", 1, KOPEN_CREATE, 50, DOM_SYSTEM, &shm) == 0);
    kobj_retain(shm); CHECK(port_send_att(srv, 50, "d", 1, shm) == 0);
    kobj_release(shm); CHECK(pages_live == live0 + 1);                                    /* память держит очередь */
    kobj_release(srv);
    CHECK(pages_live == live0 && kobj_count() == objs0);
    }
    /* ожидание нескольких портов сразу */
    {
    kobj_t *a, *b, *c, *ws[3];
    char wb[8]; uint32_t wf;
    CHECK(port_open_in("wt.a", KOPEN_CREATE, 60, DOM_SYSTEM, &a) == 0 && port_open_in("wt.b", KOPEN_CREATE, 60, DOM_SYSTEM, &b) == 0 && port_open_in("wt.c", KOPEN_CREATE, 60, DOM_SYSTEM, &c) == 0);
    ws[0] = a; ws[1] = b; ws[2] = c;
    CHECK(port_wait(ws, 3, 0, 0) == KE_AGAIN);                                          /* пусто, срок 0 */
    CHECK(port_wait(ws, 0, 0, 0) == KE_ARG && port_wait(0, 1, 0, 0) == KE_ARG && port_wait(ws, 9, 0, 0) == KE_ARG);
    { kobj_t* d[2] = { a, a }; CHECK(port_wait(d, 2, 0, 0) == KE_ARG); }                   /* повтор порта */
    CHECK(port_send(b, 7, "x", 1) == 0);
    CHECK(port_wait(ws, 3, 0, 0) == 1);                                                  /* готов второй */
    CHECK(port_wait(ws, 3, 0, 0) == 1 && port_queued(b) == 1);                           /* сообщение осталось в очереди */
    CHECK(port_recv(b, wb, sizeof wb, 0, &wf, 0) == 1 && wf == 7);
    uint64_t t0 = fake_ticks;
    CHECK(port_wait(ws, 3, 50, 0) == KE_TIMEOUT && fake_ticks >= t0 + 50);               /* никто не разбудил */
    for (int i = 0; i < 3; i++) CHECK(port_waiters(ws[i]) == 0);                 /* из очередей ушли */
    g_port = c; woken = 0; while_asleep = send_hello;                                     /* пока спим, другой поток шлёт в третий порт */
    CHECK(port_wait(ws, 3, ~0ULL, 0) == 2 && woken >= 1);
    CHECK(port_recv(c, wb, sizeof wb, 0, &wf, 0) >= 0);
    for (int i = 0; i < 3; i++) CHECK(port_waiters(ws[i]) == 0);
    kobj_release(a); kobj_release(b); kobj_release(c);
    }
    printf("%s\n", fails ? "FAILED" : "kobj: names, handles, ports (order, overflow, wait/timeout/wake, kill), wait on several ports, shared memory, futex, domains, attachments passed");
    return fails != 0;
}
