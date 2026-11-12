#include "syscall_services.h"
#include "kdrv.h"
#include "kdrv_format.h"

/*
 * Transitional in-kernel KDRV syscall adapter.
 * The kernel syscall core still performs the capability gate before routing
 * here; this adapter owns only the driver-manager operation dispatch.
 */
int syscall_service_driver(uint64_t nr, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t* result) {
    if (!result) return 0;
    switch (nr) {
    case KDRV_SYS_INFO:
    case KDRV_SYS_CFG_READ:
    case KDRV_SYS_CFG_WRITE:
    case KDRV_SYS_MMIO_MAP:
    case KDRV_SYS_DMA_ALLOC:
    case KDRV_SYS_IRQ_WAIT:
    case KDRV_SYS_NETDEV:
    case KDRV_SYS_PCILIST:
        *result = (uint64_t)kdrv_sys((unsigned)nr, a1, a2, a3);
        return 1;
    default:
        return 0;
    }
}
