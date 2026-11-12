#include "syscall_services.h"

/*
 * Transitional in-kernel routing table.
 * Subsystem adapters remain kernel code until service IPC/lifecycle contracts
 * are implemented. This layer intentionally contains no subsystem headers.
 */
int syscall_service_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t* result) {
    if (!result) return 0;
    if (syscall_service_graphics(nr, a1, a2, a3, result)) return 1;
    if (syscall_service_filesystem(nr, a1, a2, a3, result)) return 1;
    if (syscall_service_network(nr, a1, a2, a3, result)) return 1;
    if (syscall_service_audio(nr, a1, a2, a3, result)) return 1;
    if (syscall_service_clock(nr, a1, a2, a3, result)) return 1;
    return 0;
}
