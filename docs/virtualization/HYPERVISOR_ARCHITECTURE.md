# KIRA OS — KIRA Hypervisor architecture and threat model

**Статус:** целевая архитектура; наличие VMM source files не доказывает наличие аппаратного гипервизора.  
**Scope:** запуск целых гостевых ОС на VT-x/VMX, AMD-V/SVM или ARM64 EL2 там, где аппаратная платформа это поддерживает.

## 1. Separation of concerns

KIRA Hypervisor is a separate virtualization subsystem, not an application runtime:
- Native/WASM/KIRA VM execute applications inside KIRA OS.
- Linux/Windows compatibility runtimes execute selected foreign user-space applications through ABI/API adapters.
- Hypervisor executes guest operating systems with virtual CPUs, guest physical memory and virtual devices.

The hypervisor may be hosted by KIRA Kernel or use a split architecture, but its privilege boundary, ownership and trust model must be explicit. Do not assume that a source file named vmm.c is a hypervisor.

## 2. Logical architecture

```text
KIRA VM Manager / user API
        ↓ capability-checked VM requests
Hypervisor control plane
        ↓
Architecture backend: Intel VMX | AMD SVM | ARM EL2
        ↓
vCPU lifecycle / VM exits / interrupt & timer virtualization
        ↓
Guest memory manager + second-stage translation (EPT/NPT/Stage-2)
        ↓
Virtual device model / host I/O mediation
        ↓
KIRA Kernel memory, scheduler, IPC, Device Model and Security
        ↓
IOMMU (VT-d / AMD IOMMU / ARM SMMU) and physical devices
```

## 3. Core contracts

- VM lifecycle: create, configure, start, pause, resume, snapshot, restore, stop, destroy.
- vCPU lifecycle: create, run, exit reason, inject interrupt, timer, affinity and teardown.
- Guest memory: allocate, map, unmap, dirty tracking, ballooning policy and invalidation.
- VM exits: validated exit reasons, bounded emulation, no unsafe guest-controlled host pointers.
- Interrupts/timers: virtual APIC/GIC, interrupt routing, virtual clock and deterministic teardown.
- Virtual devices: versioned device ABI, queues, DMA mapping, reset, hot-unplug and snapshot state.
- IOMMU: per-device/per-VM domains, map/unmap, invalidation, fault handling and quarantine.
- Snapshot/restore: versioned state, integrity, compatibility policy and atomic persistence.
- Management API: capability-bound handles, resource quotas, audit and cancellation.

## 4. Threat model

Protect:
- host kernel and hypervisor from malicious/malformed guest state;
- guest A from guest B;
- host physical memory and devices from guest DMA;
- host services and virtual device emulators from malformed requests;
- VM management API from unauthorized creation, inspection or control;
- snapshots and saved guest state from tampering and rollback abuse.

Assume guest OS and guest users are malicious. Guest-provided addresses, descriptors, queue indices, state blobs and exit parameters are untrusted.

## 5. Required controls

- Hardware feature discovery and fail-closed backend selection.
- Strict validation of VM configuration, guest memory ranges and state transitions.
- Second-stage translation and explicit ownership of guest pages.
- IOMMU isolation before assigning passthrough devices; no passthrough by default.
- Emulated devices in least-privilege processes where possible.
- Bounded VM-exit handling and rate/CPU quotas.
- Secure lifecycle cleanup after guest crash, timeout or host service failure.
- Snapshot integrity, versioning and safe restore.
- Audit all management and device assignment operations.
- No assumption that software emulation alone provides hardware isolation.

## 6. Backend strategy

### x86-64
- Intel VMX: VMXON/VMCS lifecycle, EPT, VM-entry/exit validation, virtual APIC and interrupt controls.
- AMD SVM: HSAVE/VMCB lifecycle, NPT, exit handling and interrupt virtualization.
- Keep VMX and SVM-specific state behind a common backend interface without erasing hardware differences.

### ARM64
- EL2 entry/exit and Stage-2 translation.
- GIC virtualization only when hardware and firmware support it; otherwise a documented emulation path.
- PSCI/timer virtualization and SMMU integration.
- Test architectural differences independently, not by assuming x86 behavior maps one-to-one.

## 7. Delivery sequence

1. Inventory CPU virtualization features, kernel privilege model, memory ownership, IRQ and IOMMU readiness.
2. Threat model and host/guest boundary RFC.
3. Backend interface and VM lifecycle state machine.
4. Minimal software-managed guest memory and vCPU path on one target platform.
5. Boot one minimal guest kernel with serial console.
6. Add virtual timer, interrupt controller and virtio devices.
7. Add IOMMU isolation and adversarial DMA tests before device passthrough.
8. Add snapshot/restore and crash containment.
9. Port and verify second architecture backend.
10. Run guest-escape, fuzz, fault-injection, stress and performance suites.

## 8. Acceptance criteria

A hardware hypervisor is not complete because VMX/SVM/EL2 instructions compile. Require:
- real guest boot and repeatable VM lifecycle;
- guest memory isolation tests;
- malformed exit/state fuzzing;
- virtual interrupt/timer correctness;
- virtual device reset and teardown;
- IOMMU/DMA isolation evidence for passthrough;
- guest crash containment and host survival;
- x86-64 and ARM64 evidence reported separately.
