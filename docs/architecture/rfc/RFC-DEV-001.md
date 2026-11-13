# RFC-DEV-001 — Device Model, device identity and lifecycle

**Status:** DRAFT — target contract and source-backed migration requirements; not an approved driver ABI.  
**Contract:** DEV-001; related contracts: ARCH-001, DRV-001, DMA-001, CAP-001, BOOT-001.  
**Source baseline:** `architecture/target-baseline-2026-11` (verify live head before implementation).  
**Owners (proposed):** HAL / Device Model, Driver Manager, Security, Memory/IOMMU.

## 1. Purpose and non-goals

Define one architecture-independent model for discovering physical devices, describing them, assigning resources, binding drivers, controlling authority, and removing or recovering devices.

The Device Model is not a driver implementation, not a package manager, and not proof that drivers already run in isolated user processes. It must serve x86-64 and ARM64 while preserving bus-specific discovery backends.

Non-goals:
- freezing a new KDRV binary ABI in this RFC;
- promising hotplug for buses that do not implement it;
- enabling DMA/MSI before per-device IOMMU isolation is real and verified;
- treating PCI BDF or a device name as an authorization token;
- silently converting a failed device into a usable one.

## 2. Current source facts and limits

Observed in the architecture branch:
- `pci.c/.h` provides PCI configuration access, device enumeration, BAR probing/assignment, capability traversal and device lookup.
- PCI configuration access is backend-based: x86-64 mechanism-1 ports or ECAM selected through `pci_use_ecam()`.
- `arch/aarch64/pci_host.c` discovers the FDT `pci-host-ecam-generic` bridge, validates ECAM coverage, parses `ranges`, maps the ECAM window and calls the common PCI core.
- `arch/aarch64/pci_host_parse.c` validates cell geometry, arithmetic overflow and signed CPU-to-PCI offsets.
- The current ARM64 host adapter deliberately rejects a nonzero `bus-range` start because the common enumerator starts at bus 0. Do not relax this guard until range-aware enumeration is implemented and tested.
- PCI inventory and KDRV driver binding are not yet one complete, generic Device Model lifecycle.
- A PCI function's BDF is a location, not a permanent identity. Bus renumbering, hot removal and replacement can reuse it.
- BAR probing and assignment do not by themselves prove resource reservation against every platform owner or safe DMA isolation.
- Open security work is tracked separately: PR #136 blocks Ring 3 DMA/MSI absent per-device IOMMU isolation; PR #137 clears firmware-enabled PCI bus mastering before ownership. Neither is equivalent to a complete IOMMU implementation.

The above describes source-level observations, not a claim of complete production Device Model support.

## 3. Core objects

### 3.1 Device descriptor

A device descriptor is kernel-owned and immutable to ordinary consumers except through explicit lifecycle transitions. It should contain:
- opaque `device_id` plus monotonically changing `generation`;
- bus type and backend identity (PCI, USB, virtio, platform/FDT, ACPI-enumerated, etc.);
- location tuple (e.g. PCI segment:bus:device:function) kept separately from identity;
- vendor/product/class/revision and bus-specific compatible identifiers;
- parent/child topology references;
- discovered resources and their validated state;
- capabilities/features and supported power states;
- firmware provenance and discovery source;
- current lifecycle state and bound driver/package identity;
- diagnostics and last failure reason.

No user-supplied name, BDF, vendor ID or package manifest field is sufficient to grant authority.

### 3.2 Resources

Resources are typed and owned by the Device Model:
- MMIO and I/O port ranges;
- interrupts (legacy, MSI, MSI-X, GIC-backed);
- DMA address space and IOMMU domain;
- bus-specific queues/endpoints;
- power/clock/reset dependencies;
- shared firmware tables or immutable configuration.

Every range must be checked for overflow, alignment, supported address width, overlap and platform reservation. Ownership is exclusive unless a resource type explicitly defines safe sharing. Drivers receive scoped handles, not arbitrary global physical access.

### 3.3 Driver binding

A binding records:
- exact device identity and generation;
- driver package identity, version, signer/provenance and target ABI;
- match rule and probe result;
- granted resource handles and rights;
- start state, health and failure reason;
- revocation and teardown completion.

A binding is invalid as soon as the device generation changes or the driver is stopped/revoked.

## 4. Lifecycle state machine

Target states:

`DISCOVERED → VALIDATED → RESOURCES_RESERVED → MATCHED → AUTHORIZED → STARTING → RUNNING`

Failure and teardown paths:

- Any validation/resource/authorization failure → `QUARANTINED` or `FAILED` with a typed reason; no driver access is granted.
- `RUNNING → QUIESCING → STOPPED → REVOKED → REMOVED`.
- A failed start must execute bounded cleanup and return all resources that were successfully acquired.
- A crash or timeout must revoke the binding before a replacement driver can receive the same device.
- Re-discovery of a reused location creates a new generation; stale handles from the previous generation must fail.
- Reset/recovery is an explicit transition with a bounded retry policy, not an implicit second start.

The exact state representation and retry policy remain open for implementation review.

## 5. Required sequence

1. **Discover:** bus backend reports raw candidate facts; it does not authorize a driver.
2. **Normalize:** common Device Model validates IDs, topology, ranges and descriptor version.
3. **Reserve:** resource manager reserves MMIO/ports/interrupts/DMA windows before exposing handles.
4. **Match:** deterministic driver matching uses signed/approved package metadata and exact bus IDs.
5. **Authorize:** Security policy decides least-privilege rights for this device instance and generation.
6. **Prepare isolation:** establish DMA/IOMMU domain and mappings before any bus mastering or MSI/MSI-X is enabled.
7. **Start:** driver receives scoped resource handles; it cannot mutate unrelated devices.
8. **Operate:** all privileged operations are mediated, bounded and auditable.
9. **Quiesce:** stop new requests, drain/abort outstanding I/O, mask interrupts and stop DMA.
10. **Revoke/release:** revoke capabilities, detach IOMMU mappings, invalidate translations, release interrupts/MMIO and references in safe order.
11. **Remove/recover:** publish lifecycle events only after authoritative state has changed.

If an IOMMU domain cannot be established or its invalidation/teardown cannot be confirmed, DMA and MSI/MSI-X remain disabled (fail closed).

## 6. Security invariants

- Device discovery is not authorization.
- Location identifiers are never treated as stable identity or capability.
- A driver can access only the device generation and resource handles explicitly granted to it.
- Unknown, stale, revoked or malformed handles fail closed.
- Bus mastering is disabled at firmware handoff and remains disabled until owner policy permits it.
- DMA/MSI require a verified per-device IOMMU domain with correct requester-ID mapping, permissions, invalidation and fault handling.
- DMA buffers have explicit pin/map/unmap ownership, cache-coherency rules and teardown semantics.
- Interrupt delivery is bound to the authorized driver and device generation; stale interrupts after removal are safely rejected.
- Driver crash, timeout or unbind cannot leave bus mastering, MSI, DMA mappings or capabilities active.
- Mind may request diagnostics or policy-approved recovery; it cannot grant device rights or write device registers directly.
- All grant, denial, reset, bind, unbind and quarantine transitions are auditable.

## 7. Cross-ISA requirements

The common descriptor and lifecycle semantics must not encode x86 PCI ports, APIC vectors, ARM FDT pointers or GIC interrupt IDs as universal values.

Architecture/bus backends translate:
- x86 PCI mechanism-1 / ACPI MCFG and interrupt routing;
- ARM64 FDT/ECAM and GIC/ITS resources;
- future USB, virtio and platform device discovery.

The normalized model carries typed interrupt/resource descriptors with backend-specific opaque data where necessary. Cache coherency and DMA address width must be explicit properties, not inferred from ISA name alone.

## 8. Error model

At minimum distinguish:
- malformed discovery data;
- unsupported bus/device;
- duplicate or conflicting identity;
- resource out of range / overlap / unavailable;
- driver mismatch / package trust failure;
- authorization denied;
- IOMMU unavailable / domain creation failed / invalidation timeout;
- driver start timeout / crash / protocol violation;
- removal during I/O;
- teardown incomplete / device quarantined.

Errors must preserve the device's last known safe state and must not silently enable partially configured resources.

## 9. Conformance and regression tests

### Discovery and identity
- PCI multifunction functions, bridges and nested buses;
- ECAM bus start/end boundaries, bus > 0, address calculation and out-of-range access;
- duplicate discovery, location reuse and generation increment;
- malformed config responses, invalid FDT ranges, arithmetic overflow and truncated properties;
- disabled firmware nodes and unsupported devices.

### Resource ownership
- overlapping MMIO/port windows and reserved ranges;
- BAR alignment/size/64-bit pairing and assignment exhaustion;
- interrupt collision, masking and stale interrupt after removal;
- DMA address width, alignment, mapping overlap and cache-coherency metadata.

### Lifecycle and security
- unauthorized driver, unsigned/untrusted package and mismatched target ABI;
- no IOMMU: DMA and MSI remain disabled;
- wrong requester ID, failed invalidation and IOMMU fault;
- driver start failure after each acquired resource;
- timeout/crash during I/O, concurrent unbind and hot removal;
- stale generation handle after device replacement;
- cleanup idempotency and no leaked mapping/interrupt/resource/capability;
- Mind action request without policy approval is denied.

Tests must include host models for deterministic malformed-input coverage, but also QEMU/device-model integration tests. Host models alone do not prove real MMIO, IRQ, DMA or IOMMU behavior.

## 10. Migration plan

1. Keep current PCI APIs and behavior while extracting source ownership; do not silently change enumeration semantics.
2. Add a range-aware common PCI enumerator accepting explicit `[bus_start,bus_end]`; preserve the existing `pci_init()` wrapper only if its semantics remain unambiguous.
3. Test ECAM base calculation for nonzero bus starts and prevent access outside the mapped aperture.
4. Introduce a normalized device descriptor registry with generation-safe identity.
5. Route existing PCI and KDRV binding through explicit lifecycle transitions without changing KDRV ABI in the same patch.
6. Add resource reservation and interrupt ownership before widening driver access.
7. Integrate IOMMU domain lifecycle and firmware bus-master sanitization as mandatory gates for DMA/MSI.
8. Add USB/virtio/platform backends only after the common contract and lifecycle tests are stable.
9. Update both ISA build manifests, component inventory, dependency map, contract registry and architecture guards with each physical source slice.

## 11. Open decisions

- Exact opaque device ID generation and persistence across reboot.
- Whether topology parent references are stable handles or registry indices.
- Driver matching precedence and conflict resolution.
- Interrupt abstraction and event delivery contract.
- Resource sharing rules for bridges, multifunction devices and platform devices.
- Whether device descriptors are exposed through a service endpoint, a syscall, or both.
- Device tree/ACPI hotplug notification model and polling fallback.
- Quarantine reset authority and maximum retry/backoff policy.

## 12. Acceptance gate

DEV-001 remains Draft until:
1. owners review the descriptor and lifecycle model;
2. range-aware PCI enumeration and negative tests pass on x86-64 and ARM64;
3. resource reservation and generation-safe handles are implemented;
4. DMA/MSI cannot be enabled without a verified per-device IOMMU domain;
5. driver teardown/revoke is tested under crash, timeout and removal;
6. exact-head host, x86-64, ARM64 and QEMU integration CI passes;
7. implementation mapping and evidence are recorded in the contract registry and worklog.

No ABI freeze or production Device Model claim is made by this RFC.
