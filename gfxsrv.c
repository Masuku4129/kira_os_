#include "uaccess.h"
#include "gfxsrv.h"
#include "wm.h"
#include "vmm.h"
#include "pmm.h"
#include "input.h"
#include "cpu.h"
#include "uart.h"
#include "gfx2d.h"
#include "spinlock.h"
#include "fb.h"

#define MAX_APPS 256                  /* = наибольшее число программ (scale_procs): окно есть у каждой */
#define EVQ 32

typedef struct {
    int used;
    uint32_t pid;
    int win;                          /* id окна в оконном менеджере или -1, пока не создано */
    uint32_t w, h, pages;             /* pages - размер ОДНОГО буфера */
    uint64_t phys;                    /* передний буфер: его читает композитор; задний лежит сразу за ним и виден программе */
    char title[16];
    volatile int need_create, need_destroy, dirty;
    volatile uint32_t seq;            /* нечётный, пока идёт копия кадра в передний буфер */
    gfx_event_t q[EVQ];
    volatile uint32_t qh, qt;         /* пишет поток дисплея, читает системный вызов */
} app_t;

static app_t apps[MAX_APPS];
/* used: 0 - свободно, 2 - слот занят создающим вызовом (ещё не виден), 1 - готов. Два процесса на разных ядрах
   не получат один слот: занятие идёт под замком. Поиск видит только готовые. */
static spinlock_t gfx_lock = SPINLOCK_INIT;
/* Кто в фокусе, знает только поток дисплея (ему принадлежит оконный менеджер); остальным ядрам он публикует
   это число раз за проход, и им не нужно читать порядок окон, который в этот момент могут менять. */
static volatile int focus_pid_pub;

static app_t* find_pid(uint32_t pid) {
    for (int i = 0; i < MAX_APPS; i++) if (__atomic_load_n(&apps[i].used, __ATOMIC_ACQUIRE) == 1 && apps[i].pid == pid) return &apps[i];
    return 0;
}
static app_t* find_win(int win) {
    for (int i = 0; i < MAX_APPS; i++) if (__atomic_load_n(&apps[i].used, __ATOMIC_ACQUIRE) == 1 && apps[i].win == win && win >= 0) return &apps[i];
    return 0;
}

static void push(app_t* a, uint32_t type, uint32_t x, uint32_t y, uint32_t z) {
    uint32_t h = a->qh;
    if (h - a->qt >= EVQ) return;                     /* приложение не успевает: новые события теряем */
    a->q[h % EVQ] = (gfx_event_t){ type, x, y, z };
    __atomic_store_n(&a->qh, h + 1, __ATOMIC_RELEASE);
}

int64_t gfx_sys_create(uint32_t pid, const char* title, uint32_t w, uint32_t h) {
    const fb_info_t* fi = fb_get_info();
    uint32_t mw = fi && fi->width ? fi->width : 1024, mh = fi && fi->height ? fi->height : 768;   /* не больше экрана */
    if (pid == 0 || w < GFX_MIN_W || h < GFX_MIN_H || w > mw || h > mh) return -1;
    if ((uint64_t)w * h * 4 > GFX_USER_END - GFX_USER_VA) return -1;
    if (find_pid(pid)) return -1;                     /* одно окно на процесс */
    app_t* a = 0;
    uint64_t lf = spin_lock_irqsave(&gfx_lock);
    for (int i = 0; i < MAX_APPS; i++) if (!apps[i].used) { a = &apps[i]; a->used = 2; break; }
    spin_unlock_irqrestore(&gfx_lock, lf);
    if (!a) return -1;

    uint32_t pages = (w * h * 4 + 4095) / 4096;
    uint64_t phys = pmm_alloc_contiguous(pages * 2);  /* передний + задний буфер подряд; каждый непрерывен: композитор читает как один массив */
    if (!phys) { __atomic_store_n(&a->used, 0, __ATOMIC_RELEASE); return -1; }
    uint32_t* px = (uint32_t*)(uintptr_t)phys;
    for (uint32_t i = 0; i < w * h; i++) px[i] = 0x00101820;
    gfx_copy64((uint8_t*)px + (uint64_t)pages * 4096, px, (uint64_t)pages * 4096 / 8);

    for (uint32_t i = 0; i < pages; i++) {
        if (vmm_map_page(GFX_USER_VA + (uint64_t)i * 4096, phys + ((uint64_t)pages + i) * 4096,
                         VMM_USER | VMM_WRITE | VMM_NOEXEC | VMM_SHARED) != 0) {
            for (uint32_t k = 0; k < i; k++) vmm_unmap_page(GFX_USER_VA + (uint64_t)k * 4096);
            pmm_free_contiguous(phys, pages * 2);
            __atomic_store_n(&a->used, 0, __ATOMIC_RELEASE);
            return -1;
        }
    }
    a->pid = pid; a->win = -1; a->w = w; a->h = h; a->pages = pages; a->phys = phys;
    unsigned k = 0; while (title[k] && k < sizeof(a->title) - 1) { a->title[k] = title[k]; k++; } a->title[k] = 0;
    a->qh = a->qt = 0; a->seq = 0; a->dirty = 0; a->need_destroy = 0; a->need_create = 1;
    __atomic_store_n(&a->used, 1, __ATOMIC_RELEASE);
    return (int64_t)GFX_USER_VA;
}

int64_t gfx_sys_present(uint32_t pid) {
    app_t* a = find_pid(pid);
    if (!a) return -1;
    /* Программа рисует в задний буфер; композитор видит только готовые кадры. Писатель один (поток процесса-
       владельца), читатель - композитор, возможно на другом ядре: счётчик seq нечётен, пока идёт копия, и
       композитор перечитывает кадр (wm.c). Прерывания выключены, чтобы копию не прервало вытеснение. Обе записи
       seq - с полным барьером (xchg), поэтому слабо упорядоченные записи копии не обгоняют чётное значение. */
    uint64_t f = irq_save();
    __atomic_store_n(&a->seq, a->seq + 1, __ATOMIC_SEQ_CST);
    gfx_copy64((void*)(uintptr_t)a->phys, (const void*)(uintptr_t)(a->phys + (uint64_t)a->pages * 4096), (uint64_t)a->pages * 4096 / 8);
    __atomic_store_n(&a->seq, a->seq + 1, __ATOMIC_SEQ_CST);
    irq_restore(f);
    a->dirty = 1;
    return 0;
}

int gfx_focus_pid(void) { return __atomic_load_n(&focus_pid_pub, __ATOMIC_ACQUIRE); }

int64_t gfx_sys_event(uint32_t pid, uint64_t user_ptr) {
    app_t* a = find_pid(pid);
    if (!a || !user_range(user_ptr, sizeof(gfx_event_t), 1)) return -1;
    uint32_t t = a->qt;
    if (t != __atomic_load_n(&a->qh, __ATOMIC_ACQUIRE)) {
        gfx_event_t e = a->q[t % EVQ];
        if (copy_to_user(user_ptr, &e, sizeof e) != 0) return -1;     /* событие остаётся в очереди */
        __atomic_store_n(&a->qt, t + 1, __ATOMIC_RELEASE);
        return 1;
    }
    if (gfx_focus_pid() == (int)pid) {                 /* клавиатура уходит окну, которое сверху */
        input_event_t ev;
        if (input_pop(&ev)) {
            gfx_event_t e = { 0 };
            e.type = ev.type == INPUT_KEY_DOWN ? GEV_KEY_DOWN : GEV_KEY_UP;
            e.a = ev.key; e.b = ev.ch; e.c = ev.mods;
            return copy_to_user(user_ptr, &e, sizeof e) == 0 ? 1 : -1;
        }
    }
    return 0;
}

void gfx_proc_gone(uint32_t pid) {
    app_t* a = find_pid(pid);
    if (a) a->need_destroy = 1;                        /* окно и память освободит поток дисплея */
}

void gfx_on_input(int win, int type, int x, int y, unsigned buttons) {
    app_t* a = find_win(win);
    if (a) push(a, type == 3 ? GEV_MOUSE_MOVE : GEV_MOUSE_BUTTON, (uint32_t)x, (uint32_t)y, buttons);
}

void gfx_on_close(int win) {
    app_t* a = find_win(win);
    if (a) push(a, GEV_CLOSE, 0, 0, 0);
}

void gfx_server_run(void) {
    for (int i = 0; i < MAX_APPS; i++) {
        app_t* a = &apps[i];
        if (__atomic_load_n(&a->used, __ATOMIC_ACQUIRE) != 1) continue;
        if (a->need_destroy) {
            if (a->win >= 0) wm_destroy(a->win);
            pmm_free_contiguous(a->phys, a->pages * 2);    /* страницы помечены SHARED: процесс их не освобождал */
            a->win = -1;
            a->need_destroy = 0; a->need_create = 0;
            __atomic_store_n(&a->used, 0, __ATOMIC_RELEASE);
            continue;
        }
        if (a->need_create) {
            a->win = wm_create_user(a->title, WM_AUTO, WM_AUTO, (int)a->w, (int)a->h, (uint32_t*)(uintptr_t)a->phys);
            a->need_create = 0;
            if (a->win < 0) { uart_print("[KIRA GFX] no free window slot for '"); uart_print(a->title); uart_print("'.\n"); }
            else { wm_set_frame_seq(a->win, &a->seq); wm_raise(a->win); }
        }
        if (a->dirty && a->win >= 0) { a->dirty = 0; wm_invalidate(a->win); }
    }
    const app_t* top = find_win(wm_top());
    __atomic_store_n(&focus_pid_pub, top ? (int)top->pid : 0, __ATOMIC_RELEASE);
}
