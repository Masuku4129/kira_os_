# ADR-0004: Ring 3 DMA and MSI require per-device IOMMU isolation

- **Status:** Proposed; fail-closed implementation under exact-head CI in Draft PR #161.
- **Date:** 2026-11-14
- **Owners:** KIRA OS kernel / driver security

## Context

A Ring 3 driver runs in a restricted CPU address space, but a PCI device with bus mastering does not inherit that process's page-table restrictions. If the device can issue DMA without hardware translation/isolation, it can read or write arbitrary physical memory.

MSI/MSI-X is also a device-originated memory write. Enabling MSI requires bus mastering and therefore cannot be treated as an interrupt-only capability independent of DMA isolation.

A driver-owned DMA buffer, user confirmation, package hash, cache-coherent memory or non-coherent bounce pool does not establish a per-device DMA address-space boundary.

## Decision

1. Ring 3 drivers must not receive `CAP_DMA` or `CAP_IRQ` unless the kernel has created, attached and verified an IOMMU domain for that specific PCI requester identity.
2. Until that complete domain lifecycle exists, the policy is fail-closed: deny either capability.
3. On denial, disable MSI/MSI-X and clear PCI IO, MEM and BUS_MASTER command bits before the driver process is spawned; read back the command and capability controls to verify quiescence. A failed readback is a critical device-state failure and must be reported distinctly.
4. MMIO-only Ring 3 drivers may remain available under their independent capability checks.
5. A future IOMMU implementation may enable the capabilities only after requester-ID mapping, domain attach, translation-table invalidation, DMA mapping/unmapping, detach, fault handling, teardown and reset paths are tested.
6. VT-d and ARM SMMUv3 are separate hardware implementations but must satisfy one architecture-independent isolation contract.

## Consequences

- Current main has no complete IOMMU domain lifecycle, so production policy must deny DMA/MSI for Ring 3 drivers.
- Built-in kernel drivers remain a distinct trust class; this ADR does not claim that they are DMA-isolated.
- Firmware-enabled bus mastering must be sanitized during PCI discovery/ownership handoff as a complementary measure (tracked separately by PR #137 and follow-up).
- No software-only capability check can be described as IOMMU isolation.
- Existing DMA-dependent Ring 3 packages must be converted to MMIO-only if they can operate safely without DMA/MSI, otherwise remain unavailable until IOMMU support is accepted.

## Verification requirements

- Host policy tests deny DMA, IRQ and combined rights when isolation is absent and allow them only when a verified isolation predicate is supplied.
- x86-64 and ARM64 QEMU tests prove denied packages do not spawn and MMIO-only packages still start/stop/restart without DMA/MSI.
- PCI config readback confirms bus mastering and MSI/MSI-X are disabled after denial.
- Future VT-d/SMMUv3 enablement requires adversarial DMA tests proving a device cannot access memory outside its assigned domain, including teardown/reuse and fault injection.
- Physical hardware validation is required before claiming real-device isolation support.

## Current implementation evidence

- Draft PR #161, based on the current Stage 0 main baseline, adds the fail-closed policy and test/scenario changes.
- Initial exact-head workflow `43593017450` exposed three scenario defects; the implementation now also verifies PCI quiesce readback and scenarios assert the result. Latest exact-head CI remains pending; no PASS is claimed.
- PR #161 does not implement IOMMU hardware support.
