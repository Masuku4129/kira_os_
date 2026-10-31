/* ARM64: подсистемы, которых на этой архитектуре ещё нет (перенос идёт по шагам, см. docs/PROGRESS.md).
   Это не имитация работы: функции честно сообщают «устройства/программ нет», как сообщила бы система x86
   без экрана, без USB и без запущенных программ. Каждая уходит отсюда, когда переносится настоящая подсистема:
     - экран и оболочка (display, gfxsrv) - этап «экран на ARM64» (ramfb / virtio-gpu);
     - программы (proc) - этап «программы на ARM64» (EL0, системные вызовы, адресные пространства);
     - USB - вместе с xHCI на ARM64. */
#include <stdint.h>
#include "display.h"
#include "gfxsrv.h"
#include "proc.h"
#include "usb.h"

/* без экрана: кадров нет, опозданий нет, анимации нет */
void display_get_stats(uint64_t* loops, uint64_t* late, uint64_t* worst_gap_ns) { *loops = 0; *late = 0; *worst_gap_ns = 0; }
void display_set_fx(int level) { (void)level; }
void display_panic_flush(void) { }
int  gfx_focus_pid(void) { return 0; }

/* без программ: ни одного процесса, сбоев нет, остановить некого */
unsigned proc_stats(proc_stat_t* out, unsigned max) { (void)out; (void)max; return 0; }
uint32_t proc_faults_total(void) { return 0; }
int      proc_kill(uint32_t pid) { (void)pid; return -1; }
void     proc_set_heap_restrict(int level) { (void)level; }

/* без USB: задержанных устройств ввода не бывает */
void usb_hid_verdict(int block) { (void)block; }
