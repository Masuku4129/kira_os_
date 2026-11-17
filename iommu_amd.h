#ifndef KIRA_IOMMU_AMD_H
#define KIRA_IOMMU_AMD_H
/* AMD-Vi: ядровая часть (регистры, кольца команд и событий, домены устройств). Модель и коды - iommu.h; iommu.c передаёт сюда вызовы,
   когда платформа AMD (таблица IVRS вместо DMAR). Разбор таблицы и страницы трансляции - drivers/iommu/amd_vi.c. */
#include <stdint.h>
#include "pci.h"
#ifdef KIRA_HOST_TEST      /* тесты VT-d на хосте (tests/iommu_test.c) линкуют iommu.c без части AMD; она проверяется отдельно: tests/amdvi_test.c и QEMU */
static inline int amd_iommu_init(void) { return 0; }
static inline int amd_iommu_active(void) { return 0; }
static inline int amd_iommu_isolate(const pci_dev_t* d) { (void)d; return 0; }
static inline int amd_iommu_release(const pci_dev_t* d) { (void)d; return 0; }
static inline int amd_iommu_isolated(const pci_dev_t* d) { (void)d; return 0; }
static inline int amd_iommu_map(const pci_dev_t* d, uint64_t a, uint64_t b, uint64_t c, unsigned e) { (void)d; (void)a; (void)b; (void)c; (void)e; return 0; }
static inline int amd_iommu_unmap(const pci_dev_t* d, uint64_t a, uint64_t b) { (void)d; (void)a; (void)b; return 0; }
static inline void amd_iommu_unmap_all(const pci_dev_t* d) { (void)d; }
static inline int amd_iommu_fault_poll(void) { return 0; }
static inline uint32_t amd_iommu_fault_count(const pci_dev_t* d) { (void)d; return 0; }
#else
int  amd_iommu_init(void);                       /* число блоков, включивших трансляцию; 0 - ничего не изменилось; < 0 - таблица отвергнута */
int  amd_iommu_active(void);
int  amd_iommu_isolate(const pci_dev_t* d);
int  amd_iommu_release(const pci_dev_t* d);
int  amd_iommu_isolated(const pci_dev_t* d);
int  amd_iommu_map(const pci_dev_t* d, uint64_t iova, uint64_t phys, uint64_t len, unsigned rw);
int  amd_iommu_unmap(const pci_dev_t* d, uint64_t iova, uint64_t len);
void amd_iommu_unmap_all(const pci_dev_t* d);
int  amd_iommu_fault_poll(void);
uint32_t amd_iommu_fault_count(const pci_dev_t* d);
#endif
#endif
