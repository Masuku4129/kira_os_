#ifndef KIRA_IOMMU_SMMU_H
#define KIRA_IOMMU_SMMU_H
/* ARM SMMUv3 для ARM64: функции iommu.h. Платформа сообщает SMMU из дерева устройств до первого опроса дисков (iommu_smmu_register). */
#include <stdint.h>
typedef struct { uint32_t rid_base, sid_base, len; } smmu_rid_map_t;     /* iommu-map узла PCI: RID (шина:устройство.функция) -> StreamID */
#define SMMU_MAX_RID_MAPS 4
/* base - физический адрес регистров; coherent - обходчик таблиц видит кэши ЦП (иначе SMMU не включается); foreign - сколько ещё устройств
   (не PCI) за этим SMMU: их потоки таблица не описывает, поэтому при foreign > 0 SMMU не включается;
   msi_doorbell - физический адрес, в который устройство пишет сообщение прерывания (GITS_TRANSLATER или MSI_SETSPI_NS), 0 - сообщений нет: страница с ним
   отображается в каждый домен как окно MMIO, иначе прерывания изолированного устройства остановил бы сам SMMU. */
void iommu_smmu_register(uint64_t base, int coherent, unsigned foreign, const smmu_rid_map_t* maps, unsigned nmaps, uint64_t msi_doorbell);
#endif
