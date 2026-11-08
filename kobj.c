#include "kobj.h"
#include "heap.h"
#include "pmm.h"
#include "sched.h"
#include "spinlock.h"
#include "timer.h"

typedef struct waiter { thread_t* t; struct waiter* next; int queued; } waiter_t;
typedef struct { uint32_t pid, len; uint8_t* data; } msg_t;

typedef struct {
    kobj_t hdr;
    spinlock_t lk;
    msg_t q[PORT_QUEUE];
    uint32_t qh, qt;
    waiter_t* waiters;
} port_t;

typedef struct {
    kobj_t hdr;
    uint32_t pages;
    uint64_t* frames;
} shm_t;

static kobj_t** reg;                 // реестр именованных объектов: растёт вдвое
static unsigned reg_n, reg_cap;
static spinlock_t reg_lock = SPINLOCK_INIT;
static volatile uint64_t msgs_total;
uint64_t kobj_messages_total(void) { return msgs_total; }

// ---- имена и дескрипторы ----
int kname_ok(const char* s) {
    unsigned n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (n >= KNAME_MAX) return 0;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
    }
    return n > 0;
}

static int name_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int htab_add(htab_t* t, kobj_t* o) {
    for (uint32_t i = 0; i < t->cap; i++) if (!t->obj[i]) { t->obj[i] = o; return (int)i + 1; }
    if (t->cap >= KH_LIMIT) return KE_LIMIT;
    uint32_t nc = t->cap ? t->cap * 2 : 16;
    kobj_t** n = (kobj_t**)kmalloc(sizeof(kobj_t*) * nc);
    if (!n) return KE_NOMEM;
    for (uint32_t i = 0; i < nc; i++) n[i] = i < t->cap ? t->obj[i] : 0;
    kfree(t->obj);
    uint32_t at = t->cap;
    t->obj = n; t->cap = nc;
    t->obj[at] = o;
    return (int)at + 1;
}

void htab_free(htab_t* t) { kfree(t->obj); t->obj = 0; t->cap = 0; }
uint32_t htab_cap(const htab_t* t) { return t->cap; }

kobj_t* htab_get(htab_t* t, int h, uint32_t type) {
    if (h < 1 || (uint32_t)h > t->cap) return 0;
    kobj_t* o = t->obj[h - 1];
    return (o && (!type || o->type == type)) ? o : 0;
}

kobj_t* htab_take(htab_t* t, int h) {
    if (h < 1 || (uint32_t)h > t->cap) return 0;
    kobj_t* o = t->obj[h - 1];
    t->obj[h - 1] = 0;
    return o;
}

// ---- реестр и ссылки ----
static kobj_t* find_locked(uint32_t type, const char* name) {
    for (unsigned i = 0; i < reg_n; i++) if (reg[i]->type == type && name_eq(reg[i]->name, name)) return reg[i];
    return 0;
}

static void destroy(kobj_t* o) {
    if (o->type == KOBJ_PORT) {
        port_t* p = (port_t*)o;
        while (p->qt != p->qh) { kfree(p->q[p->qt % PORT_QUEUE].data); p->qt++; }
    } else if (o->type == KOBJ_SHM) {
        shm_t* s = (shm_t*)o;
        for (uint32_t i = 0; i < s->pages; i++) if (s->frames[i]) pmm_free_page(s->frames[i]);
        kfree(s->frames);
    }
    kfree(o);
}

void kobj_retain(kobj_t* o) {
    uint64_t f = spin_lock_irqsave(&reg_lock);
    o->refs++;
    spin_unlock_irqrestore(&reg_lock, f);
}

void kobj_release(kobj_t* o) {
    if (!o) return;
    uint64_t f = spin_lock_irqsave(&reg_lock);
    uint32_t r = --o->refs;
    if (r == 0) {
        for (unsigned i = 0; i < reg_n; i++) if (reg[i] == o) { reg[i] = reg[--reg_n]; break; }
    }
    spin_unlock_irqrestore(&reg_lock, f);
    if (r == 0) destroy(o);
}

unsigned kobj_count(void) { return reg_n; }

// Найти или создать (создание - вне замка: страницы разделяемой памяти обнуляются долго). made - заготовка
// нового объекта или 0; если объект за это время создал кто-то другой, заготовка уничтожается.
static int publish(uint32_t type, const char* name, int flags, kobj_t* made, kobj_t** out) {
    uint64_t f = spin_lock_irqsave(&reg_lock);
    while (made && reg_n >= reg_cap) {                    // реестр полон: вырастить вне замка и повторить
        unsigned want = reg_cap ? reg_cap * 2 : 64;
        spin_unlock_irqrestore(&reg_lock, f);
        kobj_t** n = (kobj_t**)kmalloc(sizeof(kobj_t*) * want);
        f = spin_lock_irqsave(&reg_lock);
        if (!n) { spin_unlock_irqrestore(&reg_lock, f); destroy(made); return KE_NOMEM; }
        if (want > reg_cap) {
            for (unsigned i = 0; i < reg_n; i++) n[i] = reg[i];
            kobj_t** old = reg; reg = n; reg_cap = want;
            spin_unlock_irqrestore(&reg_lock, f); kfree(old); f = spin_lock_irqsave(&reg_lock);
        } else { spin_unlock_irqrestore(&reg_lock, f); kfree(n); f = spin_lock_irqsave(&reg_lock); }
    }
    kobj_t* o = find_locked(type, name);
    int rc = 0;
    if (o) {
        if ((flags & KOPEN_CREATE) && (flags & KOPEN_EXCL)) rc = KE_EXIST;
        else { o->refs++; *out = o; }
    } else if (!made) {
        rc = KE_NOENT;
    } else {
        made->refs = 1;
        reg[reg_n++] = made;
        *out = made;
        made = 0;
    }
    spin_unlock_irqrestore(&reg_lock, f);
    if (made) destroy(made);
    return rc;
}

static int exists(uint32_t type, const char* name) {
    uint64_t f = spin_lock_irqsave(&reg_lock);
    int e = find_locked(type, name) != 0;
    spin_unlock_irqrestore(&reg_lock, f);
    return e;
}

static void set_name(kobj_t* o, uint32_t type, const char* name) {
    o->type = type;
    unsigned i = 0;
    for (; name[i] && i < KNAME_MAX; i++) o->name[i] = name[i];
    o->name[i] = 0;
}

// ---- порты ----
int port_open(const char* name, int flags, kobj_t** out) {
    if (!kname_ok(name)) return KE_ARG;
    port_t* p = 0;
    if ((flags & KOPEN_CREATE) && !exists(KOBJ_PORT, name)) {
        p = (port_t*)kmalloc(sizeof(port_t));
        if (!p) return KE_NOMEM;
        __builtin_memset(p, 0, sizeof *p);
        set_name(&p->hdr, KOBJ_PORT, name);
    }
    return publish(KOBJ_PORT, name, flags, p ? &p->hdr : 0, out);
}

int port_send(kobj_t* o, uint32_t from_pid, const void* data, uint32_t len) {
    if (!o || o->type != KOBJ_PORT || len > PORT_MSG_MAX) return KE_ARG;
    port_t* p = (port_t*)o;
    uint8_t* copy = 0;
    if (len) {
        copy = (uint8_t*)kmalloc(len);
        if (!copy) return KE_NOMEM;
        __builtin_memcpy(copy, data, len);
    }
    uint64_t f = spin_lock_irqsave(&p->lk);
    if (p->qh - p->qt >= PORT_QUEUE) {
        spin_unlock_irqrestore(&p->lk, f);
        kfree(copy);
        return KE_FULL;
    }
    p->q[p->qh % PORT_QUEUE] = (msg_t){ from_pid, len, copy };
    p->qh++;
    __atomic_add_fetch(&msgs_total, 1, __ATOMIC_RELAXED);
    waiter_t* w = p->waiters;                     // будим одного ждущего: сообщение одно
    if (w) {
        p->waiters = w->next;
        w->queued = 0;
        sched_wake_waiter(w->t);
    }
    spin_unlock_irqrestore(&p->lk, f);
    return 0;
}

unsigned port_queued(kobj_t* o) {
    port_t* p = (port_t*)o;
    return p->qh - p->qt;
}

int64_t port_recv(kobj_t* o, void* kbuf, uint32_t cap, uint64_t timeout, uint32_t* from_pid, int (*stop)(void)) {
    if (!o || o->type != KOBJ_PORT) return KE_ARG;
    port_t* p = (port_t*)o;
    uint64_t start = timer_get_ticks();
    for (;;) {
        uint64_t f = spin_lock_irqsave(&p->lk);
        if (p->qh != p->qt) {
            msg_t m = p->q[p->qt % PORT_QUEUE];
            p->qt++;
            spin_unlock_irqrestore(&p->lk, f);
            uint32_t n = m.len < cap ? m.len : cap;
            if (n) __builtin_memcpy(kbuf, m.data, n);
            kfree(m.data);
            if (from_pid) *from_pid = m.pid;
            return (int64_t)m.len;
        }
        uint64_t waited = timer_get_ticks() - start;
        int rc = 0;
        if (timeout == 0) rc = KE_AGAIN;
        else if (stop && stop()) rc = KE_INTR;
        else if (timeout != ~0ULL && waited >= timeout) rc = KE_TIMEOUT;
        if (rc) { spin_unlock_irqrestore(&p->lk, f); return rc; }

        waiter_t w = { (thread_t*)sched_current(), 0, 1 };
        waiter_t** pp = &p->waiters;
        while (*pp) pp = &(*pp)->next;
        *pp = &w;
        sched_wait_and_unlock(&p->lk, timeout == ~0ULL ? ~0ULL : timeout - waited);
        irq_restore(f);
        f = spin_lock_irqsave(&p->lk);
        if (w.queued) {                           // проснулись по сроку или процесс завершается: уходим из очереди
            for (pp = &p->waiters; *pp; pp = &(*pp)->next) if (*pp == &w) { *pp = w.next; break; }
        }
        spin_unlock_irqrestore(&p->lk, f);
    }
}

// ---- разделяемая память ----
#ifdef KIRA_HOST_TEST
uint64_t shm_room_kb(void);                               // тест задаёт сам
#else
static uint64_t shm_room_kb(void) { return pmm_get_free_memory_kb() / 2; }
#endif
int shm_open(const char* name, uint32_t pages, int flags, kobj_t** out) {
    if (!kname_ok(name)) return KE_ARG;
    shm_t* s = 0;
    if ((flags & KOPEN_CREATE) && !exists(KOBJ_SHM, name)) {
        if (pages == 0) return KE_ARG;
        if ((uint64_t)pages * 4 > shm_room_kb()) return KE_NOMEM;   // не больше половины свободной ОЗУ
        s = (shm_t*)kmalloc(sizeof(shm_t));
        if (!s) return KE_NOMEM;
        __builtin_memset(s, 0, sizeof *s);
        set_name(&s->hdr, KOBJ_SHM, name);
        s->frames = (uint64_t*)kmalloc(sizeof(uint64_t) * pages);
        if (!s->frames) { kfree(s); return KE_NOMEM; }
        __builtin_memset(s->frames, 0, sizeof(uint64_t) * pages);   // kmalloc не обнуляет: destroy читает frames[] до конца при неполной выдаче
        s->pages = pages;
        for (uint32_t i = 0; i < pages; i++) {
            uint64_t pa = pmm_alloc_page();
            s->frames[i] = pa;
            if (!pa) { destroy(&s->hdr); return KE_NOMEM; }
            __builtin_memset((void*)(uintptr_t)pa, 0, 4096);
        }
    }
    return publish(KOBJ_SHM, name, flags, s ? &s->hdr : 0, out);
}

uint32_t shm_pages(kobj_t* o) { return o && o->type == KOBJ_SHM ? ((shm_t*)o)->pages : 0; }
uint64_t shm_frame(kobj_t* o, uint32_t i) { return (o && o->type == KOBJ_SHM && i < ((shm_t*)o)->pages) ? ((shm_t*)o)->frames[i] : 0; }
