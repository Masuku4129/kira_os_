#include "proc.h"
#include "net.h"
#include "audio.h"
#include "syscall.h"
#include "sched.h"
#include "percpu.h"
#include "vmm.h"
#include "pmm.h"
#include "cpu.h"
#include "spinlock.h"
#include "uart.h"
#include "gfxsrv.h"
#include "elf.h"
#include "life.h"
#include "uheap.h"
#include "scale.h"
#include "heap.h"
#include "kmutex.h"
#include "kobj.h"
#include "futex.h"
#include "caps.h"
#include "smp.h"

/* Адреса программы (пользовательская половина):
     0x80_0000_0000 .. 0x80_4000_0000  сегменты ELF (1 ГБ; сколько страниц - по ОЗУ машины)
     0x80_0020_0000                    страница данных (старый договор: если сегменты её не заняли)
     0x80_7C00_0000 .. 0x80_8000_0000  главный стек: 64 КБ сразу, дальше растёт сам по обращению (до 64 МБ)
     0x80_8000_0000 .. 0x81_8000_0000  окно графики (gfxsrv)
     0x82_0000_0000 .. 1 ТБ            куча (uheap)
     0x1000_0000_0000 .. 0x2000_0000_0000  разделяемая память */
#define USER_CODE_VA    USER_VA(0x0000008000000000ULL)
#define USER_CODE_END   USER_VA(0x0000008040000000ULL)
#define USER_DATA_VA    USER_VA(0x0000008000200000ULL)
#define USER_STACK_TOP  USER_VA(0x0000008080000000ULL)
#define USER_STACK_RESERVE (64ULL << 20)
#define USER_STACK_PAGES 16

#define SHM_VA_BASE      USER_VA(0x0000100000000000ULL)
#define SHM_VA_END       USER_VA(0x0000200000000000ULL)

typedef struct { kobj_t* shm; uint64_t va; uint32_t pages; } shm_map_t;

/* Процесс = адресное пространство + один или несколько потоков. Потоки делят пространство, дескрипторы и кучу.
   Процесс завершается, когда уходит последний поток; exit из любого потока (или сбой) завершает все: каждый поток
   замечает флаг dying на выходе из ядра (после системного вызова или прерывания из Ring 3) и уходит сам.
   Пространство уничтожает последний ушедший - когда ни одно ядро его уже не использует. */
typedef struct {
    int used;
    uint32_t pid;
    uint64_t root;
    uint64_t entry;
    char name[16];
    char grant[128];                   /* файл, который программе разрешено читать (выдан при запуске; "" - нет) */
    char args[128];                    /* строка аргументов запуска (run имя аргументы) */
    uint32_t heap_pages;
    volatile uint32_t nthreads;
    volatile uint32_t dying;
    int exit_code;
    uint32_t caps;                     /* права (caps.h): задаются при запуске, дальше не меняются */
    volatile uint32_t cap_denied;      /* отказов по правам: журнал - только первый */
    kmutex_t mx;                 /* таблица дескрипторов, отображения, изменения адресного пространства */
    htab_t handles;
    shm_map_t* maps; uint32_t nmaps;  /* растёт вдвое */
    uint64_t shm_next;
    volatile uint64_t syscalls;  /* системных вызовов за жизнь (для Разума: поведение программы) */
    volatile uint32_t killed_by_mind;
} proc_t;

static proc_t* procs;          /* таблица выделяется при старте по числу ядер и ОЗУ (scale_procs) */
static unsigned max_procs;
static spinlock_t proc_lock = SPINLOCK_INIT;
static uint32_t next_pid = 1;

extern void enter_user(uint64_t rip, uint64_t rsp);
extern void enter_user_arg(uint64_t rip, uint64_t rsp, uint64_t arg);

void proc_init(unsigned cpus, uint64_t ram_kb) {
    this_cpu()->active_cr3 = vmm_kernel_root();
    max_procs = scale_procs(cpus, ram_kb);
    procs = (proc_t*)kmalloc(sizeof(proc_t) * max_procs);
    if (!procs) { max_procs = 0; uart_print("[KIRA PROC] no memory for the process table: programs disabled.\n"); return; }
    __builtin_memset(procs, 0, sizeof(proc_t) * max_procs);
    uart_print("[KIRA PROC] up to "); uart_print_dec(max_procs); uart_print(" programs at once ("); uart_print_dec(cpus);
    uart_print(" cores, "); uart_print_dec(ram_kb / 1024); uart_print(" MB RAM).\n");
}

static void user_entry(void* arg) {
    proc_t* p = (proc_t*)arg;
    enter_user(p->entry, USER_STACK_TOP - 16);
    for (;;) { }
}

static void zero_page(uint64_t pa) {
    uint64_t* q = (uint64_t*)(uintptr_t)pa;
    for (unsigned i = 0; i < 512; i++) q[i] = 0;
}

static int map_fresh(uint64_t root, uint64_t va, uint64_t flags, const uint8_t* src, size_t n) {
    uint64_t pa = pmm_alloc_page();
    if (!pa) return -1;
    zero_page(pa);
    if (src && n) {
        uint8_t* d = (uint8_t*)(uintptr_t)pa;
        for (size_t i = 0; i < n; i++) d[i] = src[i];
    }
    if (vmm_space_map(root, va, pa, flags) != 0) { pmm_free_page(pa); return -1; }
    return 0;
}

/* Сколько страниц сегментов может быть у программы: окно 1 ГБ, но не больше четверти ОЗУ. */
static uint32_t max_proc_pages(void) {
    uint64_t by_ram = (uint64_t)pmm_get_total_memory_kb() / 4 / 4;
    uint64_t win = (USER_CODE_END - USER_CODE_VA) / 4096;
    return (uint32_t)(by_ram < win ? by_ram : win);
}

int proc_spawn_elf(const char* name, const void* image, size_t len) { return proc_spawn_elf_ex(name, image, len, 0, 0); }
int proc_spawn_elf_grant(const char* name, const void* image, size_t len, const char* grant) { return proc_spawn_elf_ex(name, image, len, grant, 0); }

int proc_spawn_elf_ex(const char* name, const void* image, size_t len, const char* grant, const char* args) {
    elf_info_t info;
    int e = elf_parse(image, len, USER_CODE_VA, USER_CODE_END, max_proc_pages(), &info);
    if (e != ELF_OK) {
        uart_print("[KIRA PROC] '"); uart_print(name); uart_print("' rejected: "); uart_print(elf_strerror(e)); uart_print(".\n");
        return -1;
    }

    uint64_t f = spin_lock_irqsave(&proc_lock);
    proc_t* p = 0;
    for (unsigned i = 0; i < max_procs; i++) if (!procs[i].used) { p = &procs[i]; break; }
    if (!p) { spin_unlock_irqrestore(&proc_lock, f); return -1; }
    p->used = 1;                       /* занят до конца настройки */
    p->caps = caps_for_program(name);
    p->cap_denied = 0;
    {   /* право на чтение файла - до первого потока: программа видит его с первой команды */
        unsigned i = 0;
        if (grant) for (; grant[i] && i + 1 < sizeof p->grant; i++) p->grant[i] = grant[i];
        p->grant[i] = 0;
        i = 0;
        if (args) for (; args[i] && i + 1 < sizeof p->args; i++) p->args[i] = args[i];
        p->args[i] = 0;
    }
    uint32_t pid = next_pid++;         /* под замком: процессы запускают с разных ядер */
    spin_unlock_irqrestore(&proc_lock, f);

    uint64_t root = vmm_space_create();
    if (!root) { p->used = 0; return -1; }

    const uint8_t* img = (const uint8_t*)image;
    int ok = 1;
    for (unsigned si = 0; ok && si < info.nsegs; si++) {
        const elf_seg_t* s = &info.seg[si];
        uint64_t flags = s->exec ? 0 : (VMM_NOEXEC | (s->write ? VMM_WRITE : 0));       /* код R-X, данные RW-NX, константы R--NX */
        for (uint64_t off = 0; ok && off < s->mem_size; off += 4096) {
            size_t n = off < s->file_size ? (size_t)(s->file_size - off < 4096 ? s->file_size - off : 4096) : 0;
            ok = map_fresh(root, s->vaddr + off, flags, img + s->file_off + off, n) == 0;
        }
    }
    int data_free = 1;
    for (unsigned si = 0; si < info.nsegs; si++)
        if (USER_DATA_VA + 4096 > info.seg[si].vaddr && USER_DATA_VA < info.seg[si].vaddr + ((info.seg[si].mem_size + 4095) & ~4095ULL)) data_free = 0;
    if (ok && data_free) ok = map_fresh(root, USER_DATA_VA, VMM_WRITE | VMM_NOEXEC, 0, 0) == 0;
    for (unsigned i = 0; ok && i < USER_STACK_PAGES; i++)
        ok = map_fresh(root, USER_STACK_TOP - (i + 1) * 4096ULL, VMM_WRITE | VMM_NOEXEC, 0, 0) == 0;
    if (!ok) { vmm_space_destroy(root); p->used = 0; return -1; }

    p->root = root;
    p->heap_pages = 0;
    p->entry = info.entry;
    p->pid = pid;
    p->nthreads = 1;
    p->dying = 0;
    p->exit_code = 0;
    { kmutex_t z = KMUTEX_INIT; p->mx = z; }
    __builtin_memset(&p->handles, 0, sizeof p->handles);
    p->maps = 0; p->nmaps = 0;
    p->shm_next = SHM_VA_BASE;
    p->syscalls = 0;
    p->killed_by_mind = 0;
    unsigned k = 0;
    while (name[k] && k < sizeof(p->name) - 1) { p->name[k] = name[k]; k++; }
    p->name[k] = 0;

    thread_t* t = sched_create_proc(p->name, user_entry, p, PRIO_NORMAL, root, p);
    if (!t) { vmm_space_destroy(root); p->used = 0; return -1; }
    t->pid = p->pid;

    uart_print("[KIRA PROC] pid "); uart_print_dec(p->pid);
    uart_print(" '"); uart_print(p->name);
    uart_print("' started from ELF: Ring 3, own address space, "); uart_print_dec(info.pages);
    uart_print(" page(s) of segments, W^X enforced, stack RW-NX.\n");
    return (int)p->pid;
}

static proc_t* current_proc(void) {
    const thread_t* t = sched_current();
    return t ? (proc_t*)t->proc : 0;
}

/* Рост кучи текущей программы (системный вызов SYS_HEAP). Возвращает адрес нового куска или код uheap. */
static int64_t heap_grow_locked(proc_t* p, uint64_t pages);

int64_t proc_heap_grow(uint64_t pages) {
    proc_t* p = current_proc();
    if (!p) return UHEAP_E_ARG;
    kmutex_lock(&p->mx);                       /* потоки процесса растят одну кучу */
    int64_t r = heap_grow_locked(p, pages);
    kmutex_unlock(&p->mx);
    return r;
}

static volatile int heap_restrict;      /* 0 - обычно; 2 - Разум видит нехватку памяти: рост куч вдвое осторожнее */

void proc_set_heap_restrict(int level) { heap_restrict = level; }

static int64_t heap_grow_locked(proc_t* p, uint64_t pages) {
    uint64_t free_kb = pmm_get_free_memory_kb();
    if (heap_restrict >= 2) free_kb /= 2;     /* запас ядра проверяется против половины свободной памяти */
    int rc = uheap_check(p->heap_pages, pages, free_kb, pmm_get_total_memory_kb());
    if (rc != UHEAP_OK) {
        if (rc == UHEAP_E_QUOTA || rc == UHEAP_E_MEM) life_note("deny-hp", p->name, (int)pages);
        return rc;
    }
    uint64_t start = UHEAP_VA + (uint64_t)p->heap_pages * 4096ULL;
    for (uint64_t i = 0; i < pages; i++) {
        /* Если память кончилась на середине, уже выданное остаётся у процесса (вернётся при завершении), счётчик честный. */
        if (map_fresh(p->root, UHEAP_VA + (uint64_t)p->heap_pages * 4096ULL, VMM_WRITE | VMM_NOEXEC, 0, 0) != 0) return UHEAP_E_MEM;
        p->heap_pages++;
    }
    return (int64_t)start;
}

int proc_in_user_thread(void) { return current_proc() != 0; }
uint32_t proc_current_caps(void) { proc_t* p = current_proc(); return p ? p->caps : 0; }
int proc_cap_denied(uint32_t cap, uint64_t nr) {
    proc_t* p = current_proc();
    if (p && __atomic_fetch_add(&p->cap_denied, 1, __ATOMIC_RELAXED) == 0) {
        uart_print("[KIRA CAPS] process '"); uart_print(p->name); uart_print("' (pid "); uart_print_dec(p->pid);
        uart_print("): system call "); uart_print_dec(nr); uart_print(" denied, no right "); uart_print(caps_bit_name(cap)); uart_print(".\n");
    }
    return -12;                                                   /* KE_PERM */
}
const char* proc_current_name(void) { proc_t* p = current_proc(); return p ? p->name : ""; }
const char* proc_current_grant(void) { proc_t* p = current_proc(); return p ? p->grant : ""; }
const char* proc_current_args(void) { proc_t* p = current_proc(); return p ? p->args : ""; }
uint32_t proc_current_pid(void) { proc_t* p = current_proc(); return p ? p->pid : 0; }

unsigned proc_limit(void) { return max_procs; }

static volatile uint32_t faults_total;
uint32_t proc_faults_total(void) { return faults_total; }

void proc_count_syscall(void) {
    proc_t* p = current_proc();
    if (p) __atomic_add_fetch(&p->syscalls, 1, __ATOMIC_RELAXED);
}

unsigned proc_stats(proc_stat_t* out, unsigned max) {
    unsigned n = 0;
    for (unsigned i = 0; i < max_procs && n < max; i++) {
        proc_t* p = &procs[i];
        if (!__atomic_load_n(&p->used, __ATOMIC_ACQUIRE)) continue;
        proc_stat_t* o = &out[n++];
        o->pid = p->pid;
        for (unsigned k = 0; k < sizeof o->name; k++) o->name[k] = p->name[k < sizeof p->name ? k : 0];
        o->name[sizeof o->name - 1] = 0;
        o->threads = p->nthreads;
        o->heap_kb = p->heap_pages * 4;
        o->syscalls = p->syscalls;
        o->dying = p->dying;
    }
    return n;
}

/* Завершить процесс по номеру (Разум: исчерпание памяти). Потоки уходят сами на выходе из ядра;
   тот, что крутится без системных вызовов, - по ближайшему прерыванию таймера. */
int proc_kill(uint32_t pid) {
    for (unsigned i = 0; i < max_procs; i++) {
        proc_t* p = &procs[i];
        if (!__atomic_load_n(&p->used, __ATOMIC_ACQUIRE) || p->pid != pid) continue;
        if (!__atomic_exchange_n(&p->dying, 1, __ATOMIC_ACQ_REL)) { p->exit_code = -1; p->killed_by_mind = 1; }
        sched_wake_proc(p);
        return 0;
    }
    return -1;
}

uint32_t proc_alive_count(void) {
    uint32_t n = 0;
    for (unsigned i = 0; i < max_procs; i++) if (procs[i].used) n++;
    return n;
}

void proc_dump(void) {
    uart_print("\n  PID NAME            THREADS\n");
    for (unsigned i = 0; i < max_procs; i++) {
        if (!procs[i].used) continue;
        uart_print("  "); uart_print_dec(procs[i].pid);
        uart_print("   "); uart_print(procs[i].name);
        unsigned len = 0; while (procs[i].name[len]) len++;
        for (; len < 16; len++) uart_print(" ");
        uart_print_dec(procs[i].nthreads); uart_print(procs[i].dying ? " (exiting)\n" : "\n");
    }
    uart_print("\n");
}

/* Последний поток ушёл: всё, что принадлежало процессу, возвращается. Прерывания выключены, ядро уже в своём
   адресном пространстве, других потоков процесса нет ни на одном ядре. */
static void teardown(proc_t* p) {
    uint32_t pid = p->pid;
    gfx_proc_gone(pid);                    /* окно приложения снимет поток дисплея */
    net_proc_gone(pid);                    /* соединения TCP программы закрываются */
    audio_proc_gone(pid);                  /* и её поток звука */
    syscall_proc_gone(pid);                /* его незаконченная строка журнала */
    for (uint32_t h = 1; h <= htab_cap(&p->handles); h++) kobj_release(htab_take(&p->handles, (int)h));
    htab_free(&p->handles);
    for (uint32_t i = 0; i < p->nmaps; i++) if (p->maps[i].shm) { kobj_release(p->maps[i].shm); p->maps[i].shm = 0; }
    kfree(p->maps); p->maps = 0; p->nmaps = 0;
    unsigned freed = vmm_space_destroy(p->root);      /* страницы разделяемой памяти помечены SHARED - не освобождаются */
    int code = p->exit_code;
    uart_print("[KIRA PROC] pid "); uart_print_dec(pid);
    uart_print(" '"); uart_print(p->name);
    uart_print("' finished, exit code ");
    if (code >= 0) uart_print_dec((uint64_t)code); else uart_print("-1 (killed)");
    uart_print(", address space destroyed (");
    uart_print_dec(freed); uart_print(" pages returned to PMM).\n");
    if (code < 0) __atomic_add_fetch(&faults_total, 1, __ATOMIC_RELAXED);
    life_note(code >= 0 ? "exit" : "killed", p->name, code);
    __atomic_store_n(&p->used, 0, __ATOMIC_RELEASE);
}

/* Текущий поток процесса уходит. Сначала ядро переходит в своё адресное пространство, потом поток перестаёт
   считаться: последний поток уничтожает пространство, и ни одно ядро не должно в этот момент на нём стоять. */
static void __attribute__((noreturn)) thread_leave(void) {
    cpu_irq_off();
    proc_t* p = current_proc();
    if (!p) sched_exit();
    sched_detach_space();
    __atomic_store_n(&this_cpu()->active_cr3, vmm_kernel_root(), __ATOMIC_SEQ_CST);
    vmm_switch(vmm_kernel_root());
    if (__atomic_sub_fetch(&p->nthreads, 1, __ATOMIC_ACQ_REL) == 0) teardown(p);
    sched_exit();
}

/* Завершить процесс целиком (exit из любого потока, сбой): остальные потоки будятся и уходят на выходе из ядра. */
void proc_exit(int code) {
    proc_t* p = current_proc();
    if (!p) sched_exit();
    if (!__atomic_exchange_n(&p->dying, 1, __ATOMIC_ACQ_REL)) p->exit_code = code;
    sched_wake_proc(p);
    thread_leave();
}

void proc_thread_exit(int code) {
    proc_t* p = current_proc();
    if (p && __atomic_load_n(&p->nthreads, __ATOMIC_ACQUIRE) == 1 && !p->dying) p->exit_code = code;   /* последний поток - код процесса */
    thread_leave();
}

int proc_dying(void) {
    proc_t* p = current_proc();
    return p && __atomic_load_n(&p->dying, __ATOMIC_ACQUIRE);
}

void proc_return_check(void) {
    if (proc_dying()) thread_leave();
}

// ---- потоки ----
typedef struct { uint64_t rip, rsp, arg; } uthread_start_t;

static void user_thread_entry(void* a) {
    uthread_start_t s = *(uthread_start_t*)a;
    kfree(a);
    if (proc_dying()) thread_leave();
    enter_user_arg(s.rip, s.rsp, s.arg);
    for (;;) { }
}

int64_t proc_thread_create(uint64_t rip, uint64_t stack_top, uint64_t arg) {
    proc_t* p = current_proc();
    if (!p) return KE_ARG;
    if (rip < VMM_USER_BASE || rip >= VMM_USER_TOP || stack_top <= VMM_USER_BASE || stack_top > VMM_USER_TOP || (stack_top & 15))
        return KE_ARG;
    if (!vmm_user_range_ok(stack_top - 64, 64, 1)) return KE_ARG;              /* вершина стека - память программы, запись */
    uthread_start_t* s = (uthread_start_t*)kmalloc(sizeof *s);
    if (!s) return KE_NOMEM;
    s->rip = rip; s->rsp = USER_THREAD_SP(stack_top); s->arg = arg;              /* по ABI архитектуры (arch_vmm.h) */
    kmutex_lock(&p->mx);
    /* потоков у программы - сколько вмещает таблица планировщика (она по ядрам и ОЗУ), кроме запаса ядру */
    unsigned cap = sched_thread_capacity(), reserve = cap / 8 < 16 ? 16 : cap / 8;
    if (p->dying || p->nthreads + reserve >= cap) { kmutex_unlock(&p->mx); kfree(s); return p->dying ? KE_INTR : KE_LIMIT; }
    __atomic_add_fetch(&p->nthreads, 1, __ATOMIC_ACQ_REL);
    thread_t* t = sched_create_proc(p->name, user_thread_entry, s, PRIO_NORMAL, p->root, p);
    if (!t) { __atomic_sub_fetch(&p->nthreads, 1, __ATOMIC_ACQ_REL); kmutex_unlock(&p->mx); kfree(s); return KE_NOMEM; }
    t->pid = p->pid;
    {   /* новый поток получает вес и ограничение, которые Разум задал процессу */
        const thread_t* me = sched_current();
        t->weight = me->weight ? me->weight : SCHED_WEIGHT_NORMAL;
        t->limit = me->limit;
    }
    uint32_t tid = t->id;
    kmutex_unlock(&p->mx);
    return tid;
}

// ---- дескрипторы и разделяемая память ----
int64_t proc_handle_add(kobj_t* o) {
    proc_t* p = current_proc();
    if (!p) { kobj_release(o); return KE_ARG; }
    kmutex_lock(&p->mx);
    int h = htab_add(&p->handles, o);
    kmutex_unlock(&p->mx);
    if (h < 0) kobj_release(o);
    return h;
}

kobj_t* proc_handle_get(int h, uint32_t type) {             /* +1 ссылка на время вызова: другой поток может закрыть дескриптор */
    proc_t* p = current_proc();
    if (!p) return 0;
    kmutex_lock(&p->mx);
    kobj_t* o = htab_get(&p->handles, h, type);
    if (o) kobj_retain(o);
    kmutex_unlock(&p->mx);
    return o;
}

int64_t proc_handle_close(int h) {
    proc_t* p = current_proc();
    if (!p) return KE_ARG;
    kmutex_lock(&p->mx);
    kobj_t* o = htab_take(&p->handles, h);
    kmutex_unlock(&p->mx);
    if (!o) return KE_BADH;
    kobj_release(o);
    return 0;
}

int64_t proc_shm_map(int h) {
    proc_t* p = current_proc();
    if (!p) return KE_ARG;
    kmutex_lock(&p->mx);
    kobj_t* o = htab_get(&p->handles, h, KOBJ_SHM);
    int slot = -1;
    for (uint32_t i = 0; i < p->nmaps; i++) if (!p->maps[i].shm) { slot = (int)i; break; }
    if (slot < 0 && o) {                                  /* таблица отображений растёт вдвое */
        uint32_t nc = p->nmaps ? p->nmaps * 2 : 8;
        shm_map_t* n = (shm_map_t*)kmalloc(sizeof(shm_map_t) * nc);
        if (n) {
            for (uint32_t i = 0; i < nc; i++) n[i] = i < p->nmaps ? p->maps[i] : (shm_map_t){ 0, 0, 0 };
            kfree(p->maps); slot = (int)p->nmaps; p->maps = n; p->nmaps = nc;
        }
    }
    int64_t rc = 0;
    uint32_t pages = o ? shm_pages(o) : 0;
    uint64_t va = p->shm_next;
    if (!o) rc = KE_BADH;
    else if (slot < 0 || va + (uint64_t)pages * 4096 > SHM_VA_END) rc = KE_LIMIT;
    for (uint32_t i = 0; !rc && i < pages; i++) {
        if (vmm_space_map(p->root, va + (uint64_t)i * 4096, shm_frame(o, i), VMM_WRITE | VMM_NOEXEC | VMM_SHARED) != 0) {
            for (uint32_t k = 0; k < i; k++) vmm_space_unmap(p->root, va + (uint64_t)k * 4096);
            rc = KE_NOMEM;
        }
    }
    if (!rc) {
        kobj_retain(o);                                  /* отображение держит объект, даже если дескриптор закроют */
        p->maps[slot] = (shm_map_t){ o, va, pages };
        p->shm_next = va + (uint64_t)pages * 4096 + 4096;   /* страница-промежуток между отображениями */
        rc = (int64_t)va;
    }
    kmutex_unlock(&p->mx);
    return rc;
}

int64_t proc_shm_unmap(uint64_t va) {
    proc_t* p = current_proc();
    if (!p) return KE_ARG;
    kmutex_lock(&p->mx);
    int slot = -1;
    for (uint32_t i = 0; i < p->nmaps; i++) if (p->maps[i].shm && p->maps[i].va == va) { slot = (int)i; break; }
    if (slot < 0) { kmutex_unlock(&p->mx); return KE_ARG; }
    shm_map_t m = p->maps[slot];
    p->maps[slot].shm = 0;
    for (uint32_t i = 0; i < m.pages; i++) vmm_space_unmap(p->root, m.va + (uint64_t)i * 4096);
    /* Другие потоки процесса могут исполняться на других ядрах со старыми записями TLB: до освобождения страниц
       сбросить их везде, где загружено это пространство. */
    vmm_tlb_shootdown(p->root, m.va, m.pages);
    kmutex_unlock(&p->mx);
    kobj_release(m.shm);
    return 0;
}

void proc_space_lock(void) { proc_t* p = current_proc(); if (p) kmutex_lock(&p->mx); }
void proc_space_unlock(void) { proc_t* p = current_proc(); if (p) kmutex_unlock(&p->mx); }

/* Главный стек растёт по обращению: страница ниже отображённой части в пределах резерва - новая нулевая страница.
   Вызывается из обработчика #PF программы (прерывания выключены): на время работы с адресным пространством они
   включаются - мы на стеке ядра этого потока, как в системном вызове. 1 - страница добавлена, продолжить. */
int proc_stack_fault(uint64_t cr2, uint64_t err) {
    proc_t* p = current_proc();
    if (!p || (err & 1) || cr2 < USER_STACK_TOP - USER_STACK_RESERVE || cr2 >= USER_STACK_TOP) return 0;
    uint64_t total = pmm_get_total_memory_kb();
    if (pmm_get_free_memory_kb() < uheap_reserve_kb(total) + 64) return 0;   /* память на исходе: стек не растёт */
    cpu_irq_on();
    kmutex_lock(&p->mx);
    uint64_t va = cr2 & ~4095ULL;
    int ok = vmm_user_range_ok(va, 1, 1) || map_fresh(p->root, va, VMM_WRITE | VMM_NOEXEC, 0, 0) == 0;   /* другой поток мог успеть */
    kmutex_unlock(&p->mx);
    cpu_irq_off();
    return ok;
}

/* Системный вызов получил буфер на ещё не выросшей части стека (большой локальный массив, которого программа не
   касалась): страницы добавляются так же, как при обращении самой программы. 0 - диапазон готов или вне стека. */
int proc_stack_prepare(uint64_t va, uint64_t len) {
    proc_t* p = current_proc();
    if (!p || len == 0 || va + len < va) return 0;
    uint64_t lo = USER_STACK_TOP - USER_STACK_RESERVE;
    if (va + len <= lo || va >= USER_STACK_TOP) return 0;
    uint64_t a = va < lo ? lo : va, b = va + len > USER_STACK_TOP ? USER_STACK_TOP : va + len;
    uint64_t total = pmm_get_total_memory_kb();
    int rc = 0;
    kmutex_lock(&p->mx);
    for (uint64_t pg = a & ~4095ULL; pg < b && rc == 0; pg += 4096) {
        if (vmm_user_range_ok(pg, 1, 1)) continue;
        if (pmm_get_free_memory_kb() < uheap_reserve_kb(total) + 64) rc = -1;
        else if (map_fresh(p->root, pg, VMM_WRITE | VMM_NOEXEC, 0, 0) != 0) rc = -1;
    }
    kmutex_unlock(&p->mx);
    return rc;
}

void proc_fault(uint64_t vector, uint64_t err, uint64_t rip, uint64_t cr2) {
    proc_t* p = current_proc();
    uart_print("[KIRA PROC] pid "); uart_print_dec(p ? p->pid : 0);
    uart_print(" killed by CPU exception #"); uart_print_dec(vector);
    uart_print(" at RIP "); uart_print_hex(rip);
    if (vector == 14) {
        uart_print(", address "); uart_print_hex(cr2);
        uart_print((err & 1) ? " (protection violation)" : " (not present)");
        uart_print((err & 2) ? " write" : " read");
    } else {
        uart_print(", error code "); uart_print_hex(err);
    }
    uart_print(". The kernel is unaffected.\n");
    proc_exit(-1);                             /* весь процесс: его потоки делят повреждённое состояние */
}
