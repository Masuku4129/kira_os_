/* ARM64: подсистемы, которых на этой архитектуре ещё нет (перенос идёт по шагам, см. docs/PROGRESS.md).
   Это не имитация работы: функции честно сообщают «устройства/программ нет», как сообщила бы система x86
   без экрана, без USB и без запущенных программ. Каждая уходит отсюда, когда переносится настоящая подсистема:
     - экран и оболочка (display, gfxsrv) - этап «экран на ARM64» (ramfb / virtio-gpu);
     - USB - вместе с xHCI на ARM64. */
#include <stdint.h>
#include "display.h"
#include "gfxsrv.h"
#include "usb.h"

/* без экрана: кадров нет, опозданий нет, анимации нет */
void display_get_stats(uint64_t* loops, uint64_t* late, uint64_t* worst_gap_ns) { *loops = 0; *late = 0; *worst_gap_ns = 0; }
void display_set_fx(int level) { (void)level; }
void display_panic_flush(void) { }
int  gfx_focus_pid(void) { return 0; }
void gfx_set_bg_period(uint32_t ticks) { (void)ticks; }          /* окон нет - темп задавать некому */
void display_compositor_stats(uint32_t* par, uint32_t* ns_par, uint32_t* ns_ser) { *par = 0; *ns_par = 0; *ns_ser = 0; }

/* без экрана окна программ не создаются: вызовы графики отвечают ошибкой, как на x86 без оболочки */
int64_t gfx_sys_create(uint32_t pid, const char* title, uint32_t w, uint32_t h) { (void)pid; (void)title; (void)w; (void)h; return -1; }
int64_t gfx_sys_present(uint32_t pid) { (void)pid; return -1; }
int64_t gfx_sys_event(uint32_t pid, uint64_t user_ptr) { (void)pid; (void)user_ptr; return -1; }
void gfx_proc_gone(uint32_t pid) { (void)pid; }                  /* окон нет - закрывать нечего */

/* без USB: задержанных устройств ввода не бывает */
void usb_hid_verdict(int block) { (void)block; }
