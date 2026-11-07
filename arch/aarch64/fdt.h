#ifndef KIRA_FDT_H
#define KIRA_FDT_H
#include <stddef.h>
#include <stdint.h>
#define KIRA_FDT_MAX_MEMORY 64u
#define KIRA_FDT_MAX_RESERVED 64u
#define KIRA_FDT_MAX_CPUS 256u
#define KIRA_FDT_MAX_REG 8u
struct kira_fdt_region { uint64_t base,size; };
struct kira_fdt_info {
 struct kira_fdt_region memory[KIRA_FDT_MAX_MEMORY]; size_t memory_count;
 struct kira_fdt_region reserved[KIRA_FDT_MAX_RESERVED]; size_t reserved_count;
 uint64_t initrd_start,initrd_end;
 uint64_t gic_dist,gic_redist;
 uint32_t cpu_ids[KIRA_FDT_MAX_CPUS]; size_t cpu_count;
 uint32_t address_cells,size_cells;
 char bootargs[512];
};
int kira_fdt_parse(const void *blob,size_t available,struct kira_fdt_info *out);
#endif
