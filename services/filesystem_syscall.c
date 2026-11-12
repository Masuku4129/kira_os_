#include "syscall_services.h"
#include "syscall_numbers.h"
#include "fsrv.h"

int syscall_service_filesystem(uint64_t nr, uint64_t a1, uint64_t a2,
                               uint64_t a3, uint64_t* result) {
    (void)a2; (void)a3;
    if (!result || nr != SYS_FS) return 0;
    *result = (uint64_t)fsrv_syscall(a1);
    return 1;
}
