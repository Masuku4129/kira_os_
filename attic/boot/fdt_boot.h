#ifndef KIRA_FDT_BOOT_H
#define KIRA_FDT_BOOT_H
#include "fdt.h"
#include "kernel/boot/boot_handoff.h"
int kira_fdt_build_memory_regions(const struct kira_fdt_info *,
 struct kira_boot_handoff *);
#endif
