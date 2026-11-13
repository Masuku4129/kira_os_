# KIRA OS — первичная карта архитектурных зависимостей

**Базовый снимок:** main @ bbc7f7ab948e72511aed113b69043add10ec8350  
**Статус:** предварительная карта по исходникам и текущей целевой архитектуре; должна быть уточнена инвентаризацией каждого модуля.

## 1. Целевое направление зависимостей

```text
Hardware
  ↓
Architecture backends / HAL / Device Model
  ↓
Kernel mechanisms: IRQ, memory, scheduler, processes, syscalls, IPC
  ↓
Security authority: capabilities, isolation, trust, audit
  ↓
System services: storage, network, graphics, audio, devices, packages
  ↓
Application host APIs
  ↓
Native / WASM / KIRA VM / Linux Compatibility / Windows Compatibility
  ↓
Desktop and user applications
```

Это логическая схема. Security enforcement пересекает все уровни и не должен быть реализован только в одном верхнем слое.

## 2. Отдельные контуры

### KIRA Mind
Telemetry sources → read-only observation interface → model/diagnostics → bounded action request → policy/capability validation by resource owner → apply → audit/rollback.

Запрещённый путь: Mind → direct write to page tables/scheduler internals/device registers/kernel memory.

### KIRA Hypervisor
CPU virtualization extensions + kernel memory/interrupt primitives + IOMMU + Device Model + security policy → VM/vCPU/guest memory/virtual devices → guest OS.

Hypervisor не является дочерним модулем WASM/KIRA VM и не должен использовать application runtime ABI для управления guest OS.

### Windows/Linux compatibility
PE/ELF loader → guest ABI/API dispatch → guest process semantics → KIRA host API adapters → KIRA services → kernel primitives.

Compatibility runtimes не должны напрямую обходить capabilities и обращаться к аппаратуре.

### Drivers
Device discovery → Driver Manager → package/provenance/signature validation → capability grant → isolated driver process → KDRV syscall/API → mediated device resources + DMA/IOMMU → device.

### Storage and update
Block device → block API → KiraFS/VFS → package manager → update transaction → boot selection/recovery. Пользовательские данные должны иметь отдельные гарантии сохранности.

## 3. Наблюдаемые зависимости в main

| Source | Directly observed dependencies | Architectural concern to verify |
|---|---|---|
| kernel.c | includes/init calls across CPU, memory, scheduler, proc, syscall, filesystem, drivers, network, GUI, audio, Mind and watchdog | Is this only a composition root, or do unrelated subsystems rely on shared kernel internals? |
| `kernel/syscall.c` | process/scheduler/memory/IPC primitives, capability checks, and subsystem-agnostic service router | Driver Manager and user-facing service implementations remain behind adapters; adapters are still in-kernel until IPC lifecycle contracts exist. |
| proc.c | ELF parser, VMM, scheduler, caps table, handles, SHM, JIT | Process lifecycle and capability ownership need a single documented contract |
| caps.c | static application-name → capability mask mapping | Static allowlist vs future object capability authority, revocation and delegation |
| mind_sys.c | scheduler/process/memory/display/storage telemetry and control APIs | Every mutating action must pass an auditable policy gate |
| kdrv.c | PCI/MSI, process spawn, VMM/PMM, KFS, capabilities, network | Driver lifecycle, DMA isolation, device ownership and hot-unplug correctness |
| vfs.c | KiraFS + FAT volumes + per-volume locks | VFS semantics, namespace ownership and permission consistency |
| kfs.c | block API, allocator, mutex, metadata and recovery | Durability and power-cut invariants must be verified end-to-end |
| userland/wasm.c | runs in Ring 3, imports a limited KIRA host API | Runtime isolation, verifier, resource quotas and host API versioning |
| net.c | scheduler-managed network worker, device drivers and protocol stack | Service boundary, socket ownership and compatibility API mapping |

## 4. Dependency cycles and prohibited edges to detect

Static analysis should flag:
- GUI/application code importing kernel-private scheduler or page-table internals.
- Mind importing writable kernel internals instead of bounded request APIs.
- Drivers accessing devices outside their assigned KDRV binding/capabilities.
- Runtime compatibility code calling device drivers or MMIO directly.
- Filesystem/package/update code bypassing VFS/block ownership and integrity checks.
- Hypervisor guest code reaching host physical memory outside guest mappings/IOMMU policy.
- HAL/arch backends depending on GUI, Mind, package manager or userland.
- Kernel core depending on policy/UI-specific implementation details.

## 5. Integration order

1. BootInfo and architecture initialization.
2. Physical/virtual memory and per-CPU/interrupt contracts.
3. Process/thread lifecycle and syscall ABI.
4. IPC, shared memory and capability ownership.
5. Device Model, driver lifecycle and DMA/IOMMU.
6. Block/storage/VFS and crash recovery.
7. Service Manager, Event Bus, Resource Manager and observability.
8. Native/WASM/KIRA VM host APIs.
9. Linux/Windows compatibility adapters.
10. Hypervisor VM lifecycle and virtual devices.
11. Desktop/system applications and KIRA Mind control integration.

Все потоки могут готовить независимые изменения параллельно; интеграция должна соблюдать зависимости.


## 6. Implemented transitional syscall dependency boundary

The syscall core now depends on a subsystem-agnostic adapter contract. The router delegates to six per-subsystem adapters (graphics, filesystem, network, audio, clock, Driver Manager). This reduces direct dependency fan-out from `kernel/syscall.c` and gives each service-facing API a single owner. All adapters still execute in kernel context; IPC/service-process isolation remains a future gated migration. The canonical syscall ID registry is shared by kernel and both public user APIs at `kcc/include/syscall_numbers.h`.


The source layout now reflects part of this boundary: `kernel/syscall.c` owns syscall mechanism and `services/*_syscall.c` own service-facing adapters. Root headers remain temporary compatibility interfaces until the public include tree is consolidated.


## 7. ARM64 PCI host backend ownership

The FDT-described PCIe host bridge is now a separate architecture backend: `arch/aarch64/pci_host.c` owns ECAM discovery, MMIO mapping, address-window parsing and BAR assignment. `arch/aarch64/main.c` owns only the ordered boot handoff and supplies its logger. The common PCI enumeration core remains in `pci.c`; the x86 mechanism-1 host backend and explicit x86 enumeration remain separate work, gated on firmware bus-master sanitization and per-device IOMMU readiness. Do not infer x86/ARM parity from this ARM64 extraction.


## 8. AArch64 PCI host parsing contract

`arch/aarch64/main.c` owns only the ordered `arm_pci_init()` handoff. `arch/aarch64/pci_host.c` validates the FDT host node, bus-range and ECAM physical span, maps only the declared ECAM bus span and owns PCI initialization order. `arch/aarch64/pci_host_parse.c` is a pure parser for the PCI host `ranges` property; it validates PCI child cell geometry, parent/size cell counts, tuple boundaries, arithmetic bounds and representability of the signed CPU-to-PCI offset. The parser is host-testable and does not access MMIO or mutate PCI state.

The parser/build/test integration is not considered verified until exact-head ARM64 and host CI complete. This contract does not enable x86 PCI enumeration and does not remove the DMA/IOMMU security gates (F-013–F-015, F-018).


The PCI core exposes `pci_init_range(first_bus, last_bus)` for host-bridge root ranges and bounds recursive bridge traversal to the host aperture and every ancestor bridge's subordinate-bus window; `pci_init()` remains the x86-compatible bus-0 wrapper. ARM64 consumes the FDT `bus-range` through this API. The ECAM physical base must be 1-MiB aligned. Exact-head run `43405811778` passed host-tests, x86-64, ARM64/QEMU and 27/27 VM jobs on source head `8a5884c93a75801c6bfe89db08fb5abfc60ba7ed`; physical PCI hardware remains untested.


## Device Model lifecycle dependency (DEV-001 draft, 2026-11-13)

```text
Firmware / bus discovery
  -> normalized device descriptor + generation
  -> resource validation and reservation
  -> trusted driver match
  -> Security capability decision
  -> IOMMU domain + DMA/interrupt isolation (when needed)
  -> driver start with scoped resource handles
  -> bounded I/O and health monitoring
  -> quiesce -> revoke -> unmap/invalidate -> release -> remove/recover
```

Discovery is not authorization. Device location (PCI BDF) is not identity. DMA/MSI stays disabled until per-device isolation is established. This is a target dependency flow, not a claim that all boxes exist in the current implementation.

Current source slice across PRs #139 and #141: `device_model.c/.h` owns a bounded common registry, monotonic IDs, snapshot generations, typed bus identity and bus-neutral locations. The PCI adapter registers normalized descriptors; `pci_handle_t` is a compatibility alias to `device_handle_t`, and `pci_resolve()` validates through the common registry. KDRV stores `device_handle_t`, but still uses PCI-specific operations. Only PCI discovery is wired; USB/virtio/platform/ACPI adapters are not. The registry is boot-time single-writer and immutable after enumeration. Stale handles fail closed; KDRV quarantines stale bindings and retains DMA resources if quiesce cannot be confirmed. The PCI core still denies unsafe post-boot re-enumeration through the owner guard. Resource reservation, typed MMIO/IRQ/DMA ownership, lifecycle, coordinated revoke, hotplug and teardown remain open.
