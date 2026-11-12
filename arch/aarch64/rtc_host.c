#include <stdint.h>
#include "rtc_host.h"
#include "fdt.h"
#include "vmm.h"
#include "rtc.h"

/* AArch64 PL031 backend: device discovery and MMIO stay outside boot composition. */
void arm_rtc_init(const void* fdt, arm_rtc_log_fn log) {
    int node = fdt_find_compatible(fdt, -1, "arm,pl031");
    uint64_t base, size;
    if (node < 0 || fdt_reg(fdt, node, 0, &base, &size)) {
        if (log) log("[KIRA RTC] No PL031 real-time clock: date and time are not shown.\n");
        return;
    }

    volatile void* mapped = vmm_map_mmio(base, size ? size : 4096, 0);
    if (mapped) {
        uint32_t epoch = *(volatile uint32_t*)((uintptr_t)mapped);
        rtc_set_epoch((int64_t)epoch, "PL031, UTC");
    }
}
