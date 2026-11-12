# KIRA OS — source layout migration map

**Status:** normative migration plan; the source tree is not yet physically split into these directories.
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
| Level 2 — HAL/drivers | `arch/x86_64/**`, `arch/aarch64/**`, `pci.c/.h`, `usb.c/.h`, `virtio_*.c/.h`, `ata.c`, `ahci.c`, `nvme.c`, `e1000.c/.h`, `hda.c/.h`, `ac97.c`, `sb16.c`, `keyboard.c/.h`, `mouse.c/.h`, `fb.c/.h`, `fb_arm.c/.h` | Device discovery, resource ownership, DMA/IOMMU and driver lifecycle must become explicit contracts. Keep ISA-specific code behind HAL interfaces. |
| Level 3 — kernel | `kernel.c/.h` (privileged panic and bootstrap composition only), `pmm.c/.h`, `vmm.c/.h`, `heap.c/.h`, `sched.c/.h`, `proc.c/.h`, `syscall.c/.h` (entry/dispatch boundary), `futex.c/.h`, `kobj.c/.h`, `kmutex.c/.h`, `spinlock.h`, `percpu.c/.h`, `timer.c/.h`, `clock.c/.h`, `uaccess.*` | Kernel must not absorb user-facing services. Syscall dispatch should remain a thin, validated boundary; service semantics move behind IPC/host-service interfaces only after contracts exist. |
| Level 4 — security | `caps.c/.h`, `trust.c/.h`, `crypto.c/.h`, `sha256.c`, `kdrv_fmt.c`, security-relevant checks in `kdrv.c`, `proc.c`, `syscall.c` | Enforcement is cross-layer. Establish one authority and object-bound capabilities before relocating checks; do not create a second policy implementation. |
| Level 5 — intelligence | `mind.c/.h`, `mind_sys.c/.h`, `mindnet.c/.h`, `immune.c/.h`, `devhealth.c/.h`, `experience.c/.h` | Separate read-only telemetry, model/learning, proposal generation and owner-authorized actions. Existing direct callbacks are migration targets, not proof of a finished control boundary. |
| Level 6 — services | `kfs.c/.h`, `vfs.c/.h`, `fat.c/.h`, `blk.c/.h`, `part.c/.h`, `store.c/.h`, `fsrv.c/.h`, `fscmd.c/.h`, `net.c/.h`, `ip*.c/.h`, `audio.c/.h`, `display.c/.h`, `gfxsrv.c/.h`, `gfxui.c/.h`, `wm.c/.h`, `watchdog.c/.h`, `life.c/.h`, `scale.c/.h`, package/update code | Split service implementation from kernel mechanisms incrementally; preserve current boot and Limbo/QEMU behavior. Add Service Manager/Event Bus/Resource Manager only as real implementations with contracts and tests. |
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
- Reduce `kernel.c` to early boot orchestration and panic/fatal handling.
- Move ordered subsystem startup into an explicit boot coordinator with typed init results and reverse-order failure handling where safe.
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
- `syscall.c` retains privilege-boundary handling, capability checks and kernel primitives. It delegates service-facing syscall numbers to `syscall_services.c`.
- `syscall_services.c` currently adapts graphics, filesystem, TCP/UDP/network, audio and RTC operations to their existing subsystem APIs.
- Both x86-64 and ARM64 build manifests include the new translation unit.
- This is source-level separation only. Service code still executes in kernel context; there is no claim of process isolation, IPC transport or crash containment yet.
- RFC: `docs/architecture/rfc/RFC-SVC-001.md`.

**Gate:** exact-head CI must pass both ISA builds, host tests and QEMU scenarios. Follow-up extraction to IPC must preserve ABI IDs, argument semantics, capability checks, errors and blocking behavior.

- ABI number ownership was tightened after review: the canonical registry is `kcc/include/syscall_numbers.h`; kernel and both public userland headers include that exact source. The root `syscall_numbers.h` is only a kernel forwarding header.
