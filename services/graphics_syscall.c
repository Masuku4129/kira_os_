#include "syscall_services.h"
#include "syscall_numbers.h"
#include "gfxsrv.h"
#include "proc.h"

int syscall_service_graphics(uint64_t nr, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t* result) {
    (void)a3;
    if (!result) return 0;
    switch (nr) {
    case SYS_GFX_CREATE:
        proc_space_lock();
        {
            int64_t r = gfx_sys_create(proc_current_pid(), proc_current_name(),
                                       (uint32_t)a1, (uint32_t)a2);
            proc_space_unlock();
            *result = (uint64_t)r;
        }
        return 1;
    case SYS_GFX_PRESENT:
        *result = (uint64_t)gfx_sys_present(proc_current_pid());
        return 1;
    case SYS_GFX_EVENT:
        *result = (uint64_t)gfx_sys_event(proc_current_pid(), a1);
        return 1;
    default:
        return 0;
    }
}
