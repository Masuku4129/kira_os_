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
typedef struct shm_reservation { char name[KNAME_MAX + 1]; struct shm_reservation* next; } shm_reservation_t;
static shm_reservation_t* shm_creating;
static volatile uint64_t msgs_total;
#define KOBJ_F_PUBLIC 1u
#define KOPEN_FLAGS (KOPEN_CREATE | KOPEN_EXCL | KOPEN_PUBLIC)

uint64_t kobj_messages_total(void) { return __atomic_load_n(&msgs_total, __ATOMIC_RELAXED); }

static int open_flags_valid(int flags) {
    return !(flags & ~KOPEN_FLAGS) &&
           (!(flags & (KOPEN_EXCL | KOPEN_PUBLIC)) || (flags & KOPEN_CREATE));
}

// ---- имена и дескрипторы ----
int kname_ok(const char* s) {
    if (!s) return 0;
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

unsigned kobj_count(void) {
    uint64_t f = spin_lock_irqsave(&reg_lock);
    unsigned n = reg_n;
    spin_unlock_irqrestore(&reg_lock, f);
    return n;
}

// Найти или создать (создание - вне замка: страницы разделяемой памяти обнуляются долго). made - заготовка
// нового объекта или 0; если объект за это время создал кто-то другой, заготовка уничтожается.
static int publish(uint32_t type, const char* name, int flags, uint32_t owner_pid, kobj_t* made, kobj_t** out) {
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
        if (o->owner_pid != owner_pid && !(o->flags & KOBJ_F_PUBLIC)) rc = KE_PERM;
        else if ((flags & KOPEN_CREATE) && (flags & KOPEN_EXCL)) rc = KE_EXIST;
        else { o->refs++; *out = o; }
    } else if (!made) {
        rc = KE_NOENT;
    } else {
        made->owner_pid = owner_pid;
        made->flags = (flags & KOPEN_PUBLIC) ? KOBJ_F_PUBLIC : 0;
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

// Reserve a SHM name before allocating pages. Concurrent creators get KE_AGAIN
// instead of each allocating a full region and discarding all but one.
static int shm_reserve(const char* name, shm_reservation_t** out) {
    uint64_t f = spin_lock_irqsave(&reg_lock);
    if (find_locked(KOBJ_SHM, name)) {
        spin_unlock_irqrestore(&reg_lock, f);
        return 1;                       // already exists; normal permission checks follow
    }
    for (shm_reservation_t* q = shm_creating; q; q = q->next) {
        if (name_eq(q->name, name)) {
            spin_unlock_irqrestore(&reg_lock, f);
            return KE_AGAIN;
        }
    }
    spin_unlock_irqrestore(&reg_lock, f);

    shm_reservation_t* r = (shm_reservation_t*)kmalloc(sizeof(*r));
    if (!r) return KE_NOMEM;
    unsigned i = 0;
    while (name[i]) { r->name[i] = name[i]; i++; }
    r->name[i] = 0;
    r->next = 0;

    f = spin_lock_irqsave(&reg_lock);    // recheck after allocating outside the registry lock
    if (find_locked(KOBJ_SHM, name)) {
        spin_unlock_irqrestore(&reg_lock, f);
        kfree(r);
        return 1;
    }
    for (shm_reservation_t* q = shm_creating; q; q = q->next) {
        if (name_eq(q->name, name)) {
            spin_unlock_irqrestore(&reg_lock, f);
            kfree(r);
            return KE_AGAIN;
        }
    }
    r->next = shm_creating;
    shm_creating = r;
    spin_unlock_irqrestore(&reg_lock, f);
    *out = r;
    return 0;
}

static void shm_unreserve(shm_reservation_t* r) {
    if (!r) return;
    uint64_t f = spin_lock_irqsave(&reg_lock);
    shm_reservation_t** pp = &shm_creating;
    while (*pp && *pp != r) pp = &(*pp)->next;
    if (*pp) *pp = r->next;
    spin_unlock_irqrestore(&reg_lock, f);
    kfree(r);
}

static void set_name(kobj_t* o, uint32_t type, const char* name) {
    o->type = type;
    unsigned i = 0;
    for (; name[i] && i < KNAME_MAX; i++) o->name[i] = name[i];
    o->name[i] = 0;
}

// ---- порты ----
int port_open_as(const char* name, int flags, uint32_t owner_pid, kobj_t** out) {
    if (!kname_ok(name) || !out || !open_flags_valid(flags)) return KE_ARG;
    port_t* p = 0;
    if ((flags & KOPEN_CREATE) && !exists(KOBJ_PORT, name)) {
        p = (port_t*)kmalloc(sizeof(port_t));
        if (!p) return KE_NOMEM;
        __builtin_memset(p, 0, sizeof *p);
        set_name(&p->hdr, KOBJ_PORT, name);
    }
    return publish(KOBJ_PORT, name, flags, owner_pid, p ? &p->hdr : 0, out);
}

int port_open(const char* name, int flags, kobj_t** out) {
    return port_open_as(name, flags, 0, out);
}

int port_send(kobj_t* o, uint32_t from_pid, const void* data, uint32_t len) {
    if (!o || o->type != KOBJ_PORT || len > PORT_MSG_MAX || (len && !data)) return KE_ARG;
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
    if (!o || o->type != KOBJ_PORT) return 0;
    port_t* p = (port_t*)o;
    uint64_t f = spin_lock_irqsave(&p->lk);
    unsigned n = p->qh - p->qt;
    spin_unlock_irqrestore(&p->lk, f);
    return n;
}

int64_t port_recv(kobj_t* o, void* kbuf, uint32_t cap, uint64_t timeout, uint32_t* from_pid, int (*stop)(void)) {
    if (!o || o->type != KOBJ_PORT || (cap && !kbuf)) return KE_ARG;
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
int shm_open_as(const char* name, uint32_t pages, int flags, uint32_t owner_pid, kobj_t** out) {
    if (!kname_ok(name) || !out || !open_flags_valid(flags)) return KE_ARG;
    shm_t* s = 0;
    shm_reservation_t* reservation = 0;
    if (flags & KOPEN_CREATE) {
        int rr = shm_reserve(name, &reservation);
        if (rr == 1) return publish(KOBJ_SHM, name, flags, owner_pid, 0, out);
        if (rr) return rr;
        if (pages == 0) { shm_unreserve(reservation); return KE_ARG; }
        if ((uint64_t)pages * 4 > shm_room_kb()) {
            shm_unreserve(reservation);
            return KE_NOMEM;            // не больше половины свободной ОЗУ
        }
        s = (shm_t*)kmalloc(sizeof(shm_t));
        if (!s) { shm_unreserve(reservation); return KE_NOMEM; }
        __builtin_memset(s, 0, sizeof *s);
        set_name(&s->hdr, KOBJ_SHM, name);
        s->frames = (uint64_t*)kmalloc(sizeof(uint64_t) * pages);
        if (!s->frames) {
            kfree(s);
            shm_unreserve(reservation);
            return KE_NOMEM;
        }
        __builtin_memset(s->frames, 0, sizeof(uint64_t) * pages);   // kmalloc не обнуляет: destroy читает frames[] до конца при неполной выдаче
        s->pages = pages;
        for (uint32_t i = 0; i < pages; i++) {
            uint64_t pa = pmm_alloc_page();
            s->frames[i] = pa;
            if (!pa) {
                destroy(&s->hdr);
                shm_unreserve(reservation);
                return KE_NOMEM;
            }
            __builtin_memset((void*)(uintptr_t)pa, 0, 4096);
        }
    }
    int rc = publish(KOBJ_SHM, name, flags, owner_pid, s ? &s->hdr : 0, out);
    shm_unreserve(reservation);
    return rc;
}

int shm_open(const char* name, uint32_t pages, int flags, kobj_t** out) {
    return shm_open_as(name, pages, flags, 0, out);
}

uint32_t shm_pages(kobj_t* o) { return o && o->type == KOBJ_SHM ? ((shm_t*)o)->pages : 0; }
uint64_t shm_frame(kobj_t* o, uint32_t i) { return (o && o->type == KOBJ_SHM && i < ((shm_t*)o)->pages) ? ((shm_t*)o)->frames[i] : 0; }
