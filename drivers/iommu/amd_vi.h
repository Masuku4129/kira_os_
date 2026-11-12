#ifndef KIRA_AMD_VI_H
#define KIRA_AMD_VI_H
#include <stddef.h>
#include <stdint.h>
#define KIRA_AMDVI_MAX_UNITS 16u
#define KIRA_AMDVI_MAX_DEVICES 512u
#define KIRA_AMDVI_MAX_RANGES 64u
enum kira_amdvi_result { KIRA_AMDVI_OK=0,KIRA_AMDVI_INVALID=-1,KIRA_AMDVI_CAPACITY=-2,KIRA_AMDVI_CHECKSUM=-3,KIRA_AMDVI_UNSUPPORTED=-4 };
struct kira_amdvi_unit { uint64_t register_base;uint16_t segment;uint16_t first_device,last_device;uint8_t flags; };
struct kira_amdvi_device { uint16_t device_id;uint8_t flags;uint8_t used; };
struct kira_amdvi_inventory {
 struct kira_amdvi_unit units[KIRA_AMDVI_MAX_UNITS];size_t unit_count;
 struct kira_amdvi_device devices[KIRA_AMDVI_MAX_DEVICES];size_t device_count;
 uint64_t memory_ranges[KIRA_AMDVI_MAX_RANGES][2];size_t memory_range_count;
 uint8_t host_address_width;
};
struct kira_amdvi_ops { void *context;int (*reserve)(void *,uint64_t,uint64_t);void (*report)(void *,int,uint64_t); };
int kira_amdvi_parse_ivrs(const void *table,size_t length,struct kira_amdvi_inventory *out);
int kira_amdvi_reserve_exclusions(const struct kira_amdvi_inventory *,const struct kira_amdvi_ops *);
#endif
