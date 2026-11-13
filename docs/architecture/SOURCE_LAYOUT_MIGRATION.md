# KIRA OS — source layout migration map

**Status:** normative migration plan; partial physical migration is in progress. x86 boot ownership and syscall core/service adapters have been split into `arch/x86_64/`, `kernel/` and `services/`; most other modules remain in their historical paths.
**Baseline:** architecture branch at the time this document was added.
**Rule:** migrate one dependency-closed slice at a time. Do not move a file merely to make the tree look layered.

## 1. Target source layout

The eight logical levels are reflected in source ownership, not necessarily in eight binaries or processes.

```text
src/
  level1-platform/       # platform descriptions, firmware data contracts, board profiles
  level2-hal/            # architecture backends, buses, device model, hardware drivers
  level3-kernel/         # privileged primitives only: memory, scheduler, process/thread,
                         # IRQ, syscall entry, IPC transport, synchronization
  level4-security/       # capability authority, policy enforcement, trust, audit primitives
  level5-intelligence/   # KIRA Mind telemetry, models, proposals, diagnostics, recovery policy
  level6-services/       # service lifecycle, VFS/storage, network, audio, graphics, packages,
                         # updates, resource/power/event/observability services
  level7-runtimes/       # native, WASM, KIRA VM, Linux compat, Windows compat
  level8-user/           # desktop, explorer, settings, terminal, user-facing system apps
  crosscutting/
    boot/                # Boot Manager, BootInfo, installer and early boot composition
    toolchain/           # KCC, assemblers, linkers, SDK and build metadata
    hypervisor/          # separate guest-OS virtualization subsystem; not an app runtime
    contracts/           # public ABI/API headers and versioned schemas
tests/
  unit/
  integration/
  contract/
  fuzz/
  qemu/
```

This is the destination model. Existing build scripts and include paths are part of the public engineering surface; they must be migrated together with source files. During transition, old paths may remain as compatibility forwarding headers only when they contain no duplicate implementation and have an explicit removal condition.

## 2. Existing source ownership map

| Target owner | Current source paths / files | Migration notes |
|---|---|---|
| Level 1 — platform | `platform.h`, `hwprofile.c/.h`, `mbinfo.c/.h`, `acpi.c/.h`, `fdt.c/.h`, firmware parsing in `bootmap.c` | Separate immutable platform discovery data from architecture-specific accessors. |
| Level 2 — HAL/drivers | `arch/x86_64/**`, `arch/aarch64/**` (including the FDT-driven `arch/aarch64/pci_host.c` ECAM host backend), `pci.c/.h`, `usb.c/.h`, `virtio_*.c/.h`, `ata.c`, `ahci.c`, `nvme.c`, `e1000.c/.h`, `hda.c/.h`, `ac97.c`, `sb16.c`, `keyboard.c/.h`, `mouse.c/.h`, `fb.c/.h`, `fb_arm.c/.h` | Device discovery, resource ownership, DMA/IOMMU and driver lifecycle must become explicit contracts. Keep ISA-specific code behind HAL interfaces. |
| Level 3 — kernel | `kernel.h` (shared declarations), `arch/x86_64/kernel_main.c`, `arch/x86_64/kernel_boot.c` (x86 composition root), `kernel/syscall.c` (privilege-boundary dispatch and kernel primitives), `pmm.c/.h`, `vmm.c/.h`, `heap.c/.h`, `sched.c/.h`, `proc.c/.h`, `futex.c/.h`, `kobj.c/.h`, `kmutex.c/.h`, `spinlock.h`, `percpu.c/.h`, `timer.c/.h`, `clock.c/.h`, `uaccess.*` | Kernel must not absorb user-facing services. Syscall dispatch should remain a thin, validated boundary; service semantics move behind IPC/host-service interfaces only after contracts exist. |
| Level 4 — security | `caps.c/.h`, `trust.c/.h`, `crypto.c/.h`, `sha256.c`, `kdrv_fmt.c`, security-relevant checks in `kdrv.c`, `proc.c`, `kernel/syscall.c` | Enforcement is cross-layer. Establish one authority and object-bound capabilities before relocating checks; do not create a second policy implementation. |
| Level 5 — intelligence | `mind.c/.h`, `mind_sys.c/.h`, `mindnet.c/.h`, `immune.c/.h`, `devhealth.c/.h`, `experience.c/.h` | Separate read-only telemetry, model/learning, proposal generation and owner-authorized actions. Existing direct callbacks are migration targets, not proof of a finished control boundary. |
| Level 6 — services | `services/syscall_router.c` and per-subsystem `services/*_syscall.c` adapters (transitional in-kernel facades, not isolated processes), `kfs.c/.h`, `vfs.c/.h`, `fat.c/.h`, `blk.c/.h`, `part.c/.h`, `store.c/.h`, `fsrv.c/.h`, `fscmd.c/.h`, `net.c/.h`, `ip*.c/.h`, `audio.c/.h`, `display.c/.h`, `gfxsrv.c/.h`, `gfxui.c/.h`, `wm.c/.h`, `watchdog.c/.h`, `life.c/.h`, `scale.c/.h`, package/update code | Split service implementation from kernel mechanisms incrementally; preserve current boot and Limbo/QEMU behavior. Add Service Manager/Event Bus/Resource Manager only as real implementations with contracts and tests. |
| Level 7 — runtimes | `elf.c/.h`, `userland/wasm.c`, `userland/wasm_jit_*.h`, `userland/kira.h`, runtime launch paths in `proc.c`; future Linux/Windows runtime modules | Native ELF must be distinguished from Linux ELF personality. WASM remains current KVR baseline per ADR-0001. KIRA VM remains a separate target/experiment. Linux and Windows compatibility are separate runtimes. |
| Level 8 — user environment | `apps.c/.h`, `explorer.c/.h`, `fm.c/.h`, `dock.c/.h`, `console.c/.h`, `keyboard.c/.h` user-facing adapters, `icons.c/.h`, `font*.c`, userland apps | UI code must consume service/runtime APIs and must not import kernel-private memory, scheduler or device internals. |
| Cross-cutting — boot | `boot/**`, `bootmap.c/.h`, `grub.cfg`, `build_clang.sh`, `build_arm64.sh`, `run_qemu.sh`, `ci/make_flash.sh` | BootInfo RFC and both ISA handoffs gate changes. Keep boot orchestration separate from runtime service startup. |
| Cross-cutting — toolchain | `kcc/**`, `tools/**`, `tests/kcc/**` | Toolchain object/ABI contract must be versioned and tested for both targets. |
| Cross-cutting — hypervisor | No dedicated hardware guest hypervisor implementation confirmed in the baseline | Create only after VMX/SVM/EL2, second-stage memory, interrupt and IOMMU prerequisites are verified. Existing `vmm.c` is the kernel virtual-memory manager, not a hypervisor. |

The table is an ownership hypothesis derived from the current source tree, not an assertion that every listed file is already correctly layered. Files with mixed responsibilities must be split by symbol/call graph before relocation.

## 3. Migration sequence and gates

### M0 — freeze the observed baseline
- Record exact base commit, full source tree and active PR dependency graph.
- Keep the current build targets and public include paths working.
- For every source file, identify public symbols, callers, globals, lock ownership, privilege level and tests.
- No bulk file moves in this phase.

### M1 — extract the composition root
- x86-64 entry/steady-state loop is now `arch/x86_64/kernel_main.c`; x86 boot composition is `arch/x86_64/kernel_boot.c`.
- `kernel/syscall.c` is the first physically separated kernel mechanism; service-facing adapters live under `services/`.
- Remaining work: split ordered boot initialization into explicit phases with typed init results and safe failure-unwind semantics; preserve current boot order.
- Preserve boot order and optional-device fallback behavior.
- Acceptance: x86-64 and ARM64 builds; QEMU boot scenarios; Limbo smoke path; injected init failures do not leave interrupts/devices partially enabled.

### M2 — define and enforce kernel/HAL boundary
- Finalize BootInfo, architecture backend, memory, per-CPU, IRQ, syscall and Device Model contracts.
- HAL may depend on kernel primitives; kernel core must not depend on GUI, Mind, package manager or userland implementation.
- Acceptance: include/dependency checker plus x86-64/ARM64 contract tests.

### M3 — move security authority before service extraction
- Replace scattered policy decisions with one object-capability authority, while preserving fail-closed behavior.
- Drivers, services, runtimes, Mind and user apps request capabilities; none mint their own.
- Acceptance: denial, delegation, revocation, process exit, driver crash and confused-deputy tests.

### M4 — service process boundaries
- Extract storage, network, graphics, audio and device management behind versioned IPC contracts.
- Introduce Service Manager, Event Bus, Resource Manager and Observability only with lifecycle, backpressure, restart and fault-containment tests.
- Acceptance: service crash/restart without kernel panic; peer-death and malformed-message tests.

### M5 — runtime and compatibility boundaries
- Keep Native, WASM and KIRA VM distinct.
- Add Linux and Windows application runtimes as separate user-space subsystems over the same capability-mediated host services.
- Acceptance: real app vertical slices and per-ISA compatibility matrices; loader-only tests do not count as app compatibility.

### M6 — hypervisor as an independent subsystem
- Add VMX/SVM and ARM EL2 backends only after guest memory, second-stage translation, IOMMU and device ownership contracts exist.
- Acceptance: guest boot, isolation, malicious-exit fuzzing, crash containment and DMA/IOMMU evidence.

### M7 — user environment decoupling
- Desktop and apps consume service contracts; UI failure must not destabilize unrelated services.
- Acceptance: service restart, app isolation, input/graphics contract tests.

## 4. Dependency rules

The following edges are prohibited unless a reviewed RFC explicitly defines a narrow exception:

1. Level 1 platform descriptions must not depend on any higher level.
2. HAL/drivers must not depend on user UI, KIRA Mind, app runtimes or package-management policy.
3. Kernel core must not depend on userland, desktop, compatibility runtimes or policy UI.
4. Security authority may be called by every level but must not import UI/runtime implementations.
5. KIRA Mind may read typed telemetry and submit bounded proposals; it must not write scheduler, page tables, device registers or process state directly.
6. System services must not call architecture-specific register/MMIO helpers directly; use HAL/Device Model contracts.
7. Application runtimes must not access devices, kernel-private structures or arbitrary host paths directly.
8. Linux and Windows compatibility runtimes must not share guest-specific syscall/API semantics; share only common host-service adapters.
9. Hypervisor guest memory and device assignment must be mediated by kernel memory, Security and IOMMU contracts.
10. User environment must not include kernel-private headers or mutate kernel resources except through service APIs.

## 5. Build migration invariants

- Build source lists, linker scripts, generated headers, test include paths, CI scripts and packaging manifests migrate in the same change as source files.
- Never use broad search-and-replace for C include paths without inspecting all matches.
- No duplicate implementation under old and new paths.
- A compatibility forwarding header must be explicitly marked transitional and have a removal milestone.
- Every relocation must be behavior-preserving and have a clean x86-64 and ARM64 build plus relevant host/QEMU tests.
- No source move is considered complete while any build, test, tool or documentation reference points to a missing or stale path.

## 6. First implementation slice

The first code slice is **M1: extracting the boot composition root**, not moving hundreds of unrelated files. Before editing, enumerate all calls in `kernel_main`, startup dependencies and architecture-specific entry points. Keep the current `kernel_main` ABI and startup sequence stable while moving only the ordered initialization coordinator. This isolates a useful architectural seam without pretending the entire eight-level migration is complete.

## 7. First migration slice — boot/runtime separation

Implemented on the architecture branch:
- Added `kernel_boot.c` with the existing x86-64 early initialization and hardware bring-up sequence.
- Reduced `kernel.c` to panic handling and the steady-state core loop; the externally called `kernel_main(unsigned long, unsigned long)` ABI is unchanged.
- Added `kernel_boot_init` declaration to `kernel.h` and added `kernel_boot.c` to the x86-64 source list in `build_clang.sh`.
- Preserved the existing initialization order, optional display/network/audio fallback behavior, timer calibration, APIC fallback and SMP startup.
- ARM64 uses its separate `arch/aarch64/main.c` entry path and is not changed by this x86-only extraction.

**Verification status:** GitHub Actions was triggered for the architecture PR after the first source-list update. That run targets an earlier head than the final header correction; a fresh run for the final head is required. No build or runtime test is claimed PASS until that exact-head run completes.

**Next M1/M2 work:** compare the extracted boot sequence against the pre-refactor source line-by-line, then split early platform discovery from subsystem startup only after typed init/error contracts are documented. Do not add rollback of hardware initialization until each subsystem exposes a safe teardown operation.


## 8. Second migration slice — syscall core vs service adapters

Implemented as a transitional in-kernel boundary:
- `syscall_numbers.h` is now the shared numeric registry for Native syscall IDs; values remain unchanged.
- `kernel/syscall.c` retains privilege-boundary handling, capability checks and kernel primitives. It delegates service-facing syscall numbers to `services/syscall_router.c`.
- `services/syscall_router.c` currently adapts graphics, filesystem, TCP/UDP/network, audio, RTC and Driver Manager operations to their existing subsystem APIs.
- Both x86-64 and ARM64 build manifests include the new translation unit.
- This is source-level separation only. Service code still executes in kernel context; there is no claim of process isolation, IPC transport or crash containment yet.
- RFC: `docs/architecture/rfc/RFC-SVC-001.md`.

**Gate:** exact-head CI must pass both ISA builds, host tests and QEMU scenarios. Follow-up extraction to IPC must preserve ABI IDs, argument semantics, capability checks, errors and blocking behavior.

- ABI number ownership was tightened after review: the canonical registry is `kcc/include/syscall_numbers.h`; kernel and both public userland headers include that exact source. The root `syscall_numbers.h` is only a kernel forwarding header.


## 9. Architecture ownership correction — x86 kernel entry

- Moved the x86-only kernel main loop from root `kernel.c` to `arch/x86_64/kernel_main.c`.
- Moved x86 boot composition from root `kernel_boot.c` to `arch/x86_64/kernel_boot.c`.
- Scoped `kernel_boot_init` declaration to `arch/x86_64/kernel_boot.h`; the common `kernel.h` no longer exposes an x86-only function.
- Updated the x86 build's architecture source list. ARM64 continues to use `arch/aarch64/main.c` and does not compile x86 entry code.
- This change corrects source ownership; it does not yet split the boot sequence into independent platform/driver/service init phases.

### 8.1 Subsystem adapter ownership

- The central router `services/syscall_router.c` contains no subsystem-specific headers and delegates to per-subsystem translation units.
- `services/graphics_syscall.c`, `services/filesystem_syscall.c`, `services/network_syscall.c`, `services/audio_syscall.c`, and `services/clock_syscall.c` each own their syscall subset.
- `tests/architecture_test.py` checks that each service syscall has exactly one adapter owner and that both ISA builds compile the full adapter set.
- These are still in-kernel calls. The next step is typed, versioned service request contracts and IPC migration, not pretending that process isolation already exists.


## 10. Physical source ownership — kernel and services

The syscall core is located at `kernel/syscall.c`. The service router and subsystem adapters are under `services/`: `services/syscall_router.c`, `services/graphics_syscall.c`, `services/filesystem_syscall.c`, `services/network_syscall.c`, `services/audio_syscall.c`, `services/clock_syscall.c`, and `services/driver_syscall.c`.

Both x86-64 and ARM64 build manifests compile these exact paths. Root `syscall.h`, `syscall_services.h` and `syscall_numbers.h` remain compatibility/interface headers during migration. No syscall numbers or calling convention changed. This is physical ownership only; adapters still execute in kernel context.


## 11. AArch64 PCI host backend ownership

- `arch/aarch64/pci_host.c` owns FDT-described `pci-host-ecam-generic` discovery, ECAM MMIO mapping, PCI address-window parsing and BAR assignment.
- `arch/aarch64/main.c` retains the ordered boot call and supplies its serialized console logger; it no longer implements PCI host discovery or calls `pci_use_ecam()`, `pci_init()` or `pci_assign()` directly.
- The backend is built only by the ARM64 target. This extraction does not enable or change x86-64 PCI enumeration and does not bypass the pending DMA/IOMMU security gates.
- Acceptance: architecture ownership guard, both builds, host suite, ARM64 QEMU GICv2/GICv3 and PCI/driver boot scenarios on the exact head.


## 12. AArch64 PCI host FDT/ECAM validation slice

- Added `arch/aarch64/pci_host_parse.[ch]` as a pure parser for the PCI host bridge `ranges` property. PCI child addresses must use the three-cell PCI format; parent address and size cell counts are bounded to one or two cells.
- The parser rejects truncated range tuples, address-window overflow, CPU-address overflow and CPU-to-PCI offsets that cannot be represented by the existing signed `pci_window_t.cpu_off` contract. It selects the largest memory window of each supported width and does not reinterpret I/O windows as memory.
- `arch/aarch64/pci_host.c` now rejects invalid/disabled host nodes, malformed bus-range properties, bus numbers outside 0..255, ECAM regions too small for the declared bus span and overflowing physical ranges. It maps only the required ECAM span and fails closed on malformed ranges before PCI enumeration.
- Added `tests/pci_host_parse_test.c` for valid 32/64-bit windows, negative offsets, invalid cell geometry, truncated tuples and arithmetic overflow. The test is registered in `tests/run_host_tests.sh`; the parser is included in the ARM64 build and guarded by `tests/architecture_test.py`.
- **Verification:** changes are committed on the architecture branch. Fresh exact-head CI is required; no build/test PASS is claimed for this slice until the run on the final SHA completes.


### PCI host follow-up constraints

- The common PCI enumerator now accepts an explicit root bus range through `pci_init_range(first_bus, last_bus)`; `pci_init()` remains the legacy bus-0 wrapper. ARM64 passes the validated FDT `bus-range`, including a nonzero start. Recursive bridge traversal is bounded by the host aperture and each ancestor bridge's subordinate-bus window. Exact-head CI for nested-window regression passed in run `43405811778` on source head `8a5884c93a75801c6bfe89db08fb5abfc60ba7ed`. PR #141 adds the first common registry and PCI adapter; its exact-head CI remains required.
- ECAM base must be 1-MiB aligned, matching the per-bus ECAM aperture. A final exact-head run is required for this follow-up.
- Exact-head CI run `43298087855` on `618ce01344e219991323aca51f9de58c55ed2aab` completed **30/30 jobs successfully**, including host tests, x86-64/ARM64 builds and the full QEMU/Limbo matrix. This run predates the bus-start/alignment follow-up commits; those new checks remain unverified until the next run.


## 13. Initial common Device Model registry adapter (PR #141)

- Added `device_model.h/.c` as a bus-neutral, bounded registry. It owns monotonic device IDs and snapshot generations and exposes opaque `device_handle_t` resolution.
- PCI enumeration is the first production adapter and registers normalized descriptors. `pci_handle_t` remains a compatibility alias; KDRV now stores the common handle type.
- The registry is compiled in both x86-64 and ARM64 targets and covered by `tests/device_model_test.c`, the PCI range model and the virtio PCI host model.
- The registry currently has boot-time single-writer semantics. USB/virtio/platform/ACPI producers, resource ownership, lifecycle and hotplug are not part of this slice. PR #141 exact-head run `43408288041` passed host-tests, x86-64, ARM64/QEMU and 27/27 VM jobs; compliance run `43408709474` passed.
