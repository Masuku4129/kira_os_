# KIRA OS — Stage 0 component and workstream matrix (2026-11-14)

**Pinned source baseline:** `main` @ `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`  
**Status:** PRELIMINARY INVENTORY — not a component acceptance report.  
**Rule:** no area is marked READY in this matrix. Presence in source/build lists is not proof of runtime correctness or physical hardware support.

Status vocabulary:
- **PARTIAL** — production source and/or tests exist, but one or more required contracts, lifecycle, architecture parity or acceptance criteria remain open.
- **EXPERIMENTAL** — prototype/research path; not a production release guarantee.
- **BRANCH-ONLY** — exists in an unmerged PR/branch, not current main.
- **UNVERIFIED** — insufficient evidence to make a readiness claim.
- **BLOCKED** — a known safety/contract gap prevents enabling the relevant capability.

## 1. Fourteen master-plan areas A–N

| Area | Main-tree inventory | Preliminary status | Confirmed gaps / next audit |
|---|---|---|---|
| **A. Architecture & base platform** | `ARCHITECTURE.md`, `docs/MASTER_DEVELOPMENT_PLAN.md`, boot/CPU/memory/device contracts spread across source | PARTIAL | Current architecture baseline, versioned BootInfo, Native ABI, IPC, Device Model and service contracts are not all consolidated in main. Target architecture package in PR #135 is BRANCH-ONLY and heavily diverged. F-004/F-005/F-006. |
| **B. KIRA Kernel** | `kernel.c`, `pmm.c`, `vmm.c`, `proc.c`, `sched.c`, `syscall.c`, `percpu.c`, `kobj.c`, `futex.c`, x86-64 and ARM64 backends | PARTIAL | Boot and memory lifecycle, SMP, process/thread teardown, syscalls, IPC and lock ordering need a line-by-line contract audit. F-001 and F-008 are confirmed stack lifecycle/hardening gaps; x86-64 PMM release (F-009) and TLB shootdown (F-010) are verified on unmerged Draft PRs; ARM64 reserved-page ownership and atomic block release (F-013/F-014) are verified 43/43 on PR #158 but remain absent from main until integration. |
| **C. KIRA Virtual Runtime / application ABI** | `userland/wasm.c`, `userland/wasm_jit_x64.h`, `userland/wasm_jit_x64_gen.h`, module build/tests | PARTIAL | Existing WASM is a bounded subset, not full modern WASM conformance. Parser/verifier, type/index bounds, quotas, cancellation, JIT W^X and host API need independent audit. KVM1 research implementation is in PR #134/branch-only and must not be treated as production KVR; ADR-0001 keeps WASM primary. |
| **D. KIRA Mind** | `mind.c`, `mindnet.c`, `mind_sys.c`, `experience.c`, `watchdog.c` | PARTIAL | Telemetry causality, policy authority, audit/rollback, lock/latency impact and all actions that can change scheduler/resources need explicit policy-boundary review. Tests exist but do not prove safe authority boundaries. |
| **E. KCC / IR / toolchain** | `kcc/`, `userland/kcc.c`, x86-64 and ARM64 build integration | PARTIAL | Compiler/IR/ABI/version contracts, relocations, diagnostics, reproducible bootstrap, memory-safety checks and backend parity remain to be specified and tested. |
| **F. Memory safety & execution security** | `caps.c`, `uaccess.c`, `trust.c`, `immune.c`, `kdrv.c`, W^X/NX mappings, package hashes | BLOCKED for DMA/MSI; otherwise PARTIAL | No IOMMU implementation is present in main. Draft PR #161 adds a current-main fail-closed gate for Ring 3 CAP_DMA/CAP_IRQ and quiesces the PCI function before rejection; until accepted, main still has the source gap. Real per-device VT-d/SMMUv3 domains, firmware bus-master sanitization (PR #137 transplant pending), secure boot, hardware anti-rollback anchor, DMA pin/unpin and complete teardown remain open. F-002, F-009 and F-010. |
| **G. Drivers & Driver Manager** | `pci.c`, `virtio.c`, `virtio_blk.c`, `nvme.c`, `ahci.c`, `usb.c`, `xhci.c`, `uhci.c`, `usbhid.c`, `usbhub.c`, `usbmsc.c`, `e1000.c`, `virtio_net.c`, `hda.c`, `audio.c`, `kdrv.c`, `userland/drvget.c`, `userland/drvman.c` | PARTIAL | Generic bus-independent Device Model/resource lifecycle not integrated. F-003 is under current-main Draft PR #162 with nonzero-root and subordinate-window tests; exact-head CI pending. DMA/MSI fail-closed policy is under Draft PR #161 verification; DMA/MSI remains blocked until real IOMMU. Driver source acquisition is not automatic universal Windows/Linux kernel-driver translation; compatibility must be explicitly scoped and certified. |
| **H. KiraFS & storage** | `kfs.c`, `store.c`, `vfs.c`, `fat.c`, `part.c`, `blk.c`, `virtio_blk.c`, `nvme.c`, `ahci.c` | PARTIAL | KiraFS and FAT/VFS paths have substantial host tests; block lifecycle, flush/barriers, concurrent access, crash recovery on real devices, encryption/key management and format migration remain open. |
| **I. Networking & communications** | `net.c`, `ip6.c`, `ipfrag.c`, `e1000.c`, `virtio_net.c`, userland network programs | PARTIAL | Audit protocol completeness, socket API, retransmission/congestion behavior, DNS/DHCP, firewall/capability policy, DMA-safe buffers, device reset and recovery. Wi-Fi/Bluetooth are not established as supported. |
| **J. Graphics & desktop** | `display.c`, `wm.c`, `gfx2d.c`, `gfxui.c`, `gfxsrv.c`, `fb.c`, `virtio_gpu.c`, `apps.c`, `explorer.c`, `dock.c` | PARTIAL | Compositor responsiveness, scheduling/locking, 2D/3D acceleration, device failure and input/resize regression need dedicated audit. Spatial Minimal is the target design, not a completed visual implementation. PR #150 has an exact-head host failure at `tests/wm_test.c:84`; it remains branch-only. |
| **K. System services & application platform** | `fsrv.c`, `progs.c`, `kobj.c`, `life.c`, `devhealth.c`, `kpar.c`, `fscmd.c` | PARTIAL | Many service functions remain linked into kernel image/context. A separate service manager with dependency graph, capability grants, lifecycle, crash recovery and stable System API is not yet established as a complete contract. |
| **L. Boot, install & update** | `build_clang.sh`, `build_arm64.sh`, `bootmap.c`, `mbinfo.c`, GRUB ISO creation, ARM64 image creation | PARTIAL | Current build path uses GRUB; unified BootInfo and full native boot-manager/installer/A-B update/recovery contracts remain open. x86-64 `--android` profile removes TESTPROGS; do not use it when a complete test/educational ISO is required. F-004/F-007. |
| **M. Architectures & real hardware** | `arch/x86_64/`, `arch/aarch64/`, separate build scripts and QEMU tests | PARTIAL / UNVERIFIED on physical hardware | Build/QEMU evidence is not physical-hardware support. x86-64 and ARM64 behavior must be checked separately for boot, interrupt, SMP, memory, PCI, DMA/IOMMU, drivers and recovery. |
| **N. Development tools, automation & quality** | `tests/`, `ci/`, `.github/workflows/ci.yml`, build scripts, host tests and QEMU matrix | PARTIAL | Exact-SHA evidence exists for individual PRs, not a blanket pass for main. Docs consistency, complete image manifests, file-loss guard and independent architecture checks are incomplete or branch-only. PR #148 status ledger is unmerged. |

## 2. Twelve parallel workstreams

| Stream | Scope | Preliminary status | Primary dependencies / immediate gate |
|---|---|---|---|
| **1. Kernel + processes + IPC + SMP** | Boot, exceptions, scheduler, process/thread lifecycle, IPC, synchronization | PARTIAL | A, B, F, M. Close stack lifecycle F-001/F-008; integrate verified x86/ARM64 PMM F-009/F-013/F-014 and TLB F-010; complete MMIO F-011 integration (PR #160 43/43 PASS) and verify process teardown/cross-CPU TLB. |
| **2. KVR + application ABI + compatibility runtimes** | WASM, Native ABI, KIRA VM decision, Linux/Windows app compatibility | PARTIAL / EXPERIMENTAL | A, B, C, F, K. WASM audit first; no second production VM ISA without ADR. Windows/Linux kernel drivers are not app-runtime compatibility. |
| **3. KCC + IR + backends + build pipeline** | Compiler, object/link formats, x86-64/AArch64 backends, bootstrap | PARTIAL | A, E, F, N. Freeze ABI/IR contracts before expanding compiler scope. |
| **4. Mind Core + specialized models + safe control** | Telemetry, prediction, diagnosis, bounded resource control | PARTIAL | A, B, D, F, N. Policy-mediated requests only; no arbitrary kernel memory/MMIO/lock access. |
| **5. Memory safety + IOMMU + secure boot/signatures** | Ownership, W^X, DMA isolation, signing, trust and rollback | BLOCKED for DMA/MSI | B, F, G, M. Fail closed before DMA/MSI; implement VT-d and SMMUv3 lifecycle; PMM transactional block release and ARM64 reserved ownership are verified on PR #154/#158 (unmerged); MMIO F-011 is verified 43/43 on PR #160 (unmerged); integrate before treating these as main baseline. |
| **6. Driver framework + drivers + Driver Manager** | Device discovery, registry, resources, bind/unbind, drivers, acquisition pipeline | PARTIAL | A, F, G, M. DEV-001, PCI range, IOMMU, stable handles and resource ownership. |
| **7. KiraFS + storage + recovery** | Filesystems, block layer, caching, flush, crash consistency, removable media | PARTIAL | B, F, G, H, L. Block/VFS contracts and recovery/fault injection. |
| **8. Network stack + network services** | Ethernet, IPv4/IPv6, transport, sockets, TLS, policy | PARTIAL | F, G, I, K. Audit protocol semantics, DMA-safe buffers and isolation. |
| **9. Desktop/compositor + toolkit + system apps** | Display, WM, input, toolkit, desktop, apps | PARTIAL; implementation paused behind foundation | B, F, G, J, K. Do not resume visual expansion before kernel/driver/security dependencies are stable. |
| **10. BIOS/UEFI + installer + update/recovery** | Firmware boot, handoff, ISO, install, atomic update and recovery | PARTIAL | A, B, F, L, M. BootInfo contract and full-profile artifact manifest. |
| **11. x86-64 + ARM64 + HAL** | CPU/platform abstraction, interrupt/timer/memory/device backends | PARTIAL; physical hardware UNVERIFIED | A, B, F, G, M. Exact architecture parity and QEMU evidence; then physical test matrix. |
| **12. CI, fuzzing, performance and release engineering** | Host tests, sanitizers, VM matrix, reproducible artifacts, release gates | PARTIAL | All areas. Every PASS exact-SHA; test profile and artifact manifest explicit; docs guard must be merged after review. |

## 3. Bottom-up integration gates

1. **Firmware and platform handoff:** BIOS/UEFI/Multiboot2/ARM64 FDT (close F-015 parser bounds), memory map, modules, framebuffer and versioned BootInfo.
2. **CPU/HAL:** CPU features, GDT/IDT, exception frame, IRQ, timers, APIC/GIC, SMP and TLB operations.
3. **Device discovery and ownership:** PCI host range, bridges, stable handles, resources, DMA/MSI policy, IOMMU and device lifecycle.
4. **Physical/virtual memory:** PMM, VMM, page-table ownership, stack/heap guards, W^X/NX, TLB, DMA pin/unpin.
5. **Kernel execution:** processes/threads, scheduler, syscalls, user access, IPC, synchronization, teardown.
6. **Security policy:** capabilities, signatures, trust, isolation, anti-rollback, audit and recovery.
7. **Storage/network/services:** block/VFS/KiraFS, network protocols, service lifecycle and stable APIs.
8. **Execution/toolchain:** Native ABI, WASM/KVR, KCC, packages and later compatibility runtimes.
9. **Mind:** telemetry, diagnosis and policy-mediated actions.
10. **Graphics/user environment:** compositor, display, input, audio, apps and accessibility.
11. **Hypervisor:** separate hardware virtualization subsystem; do not conflate with WASM/KIRA VM.
12. **Release:** install/update/recovery, full test image, manifests, x86-64/ARM64 QEMU, Limbo and physical hardware matrix.

This is an integration order. All twelve workstreams remain visible in the project ledger; no area is silently dropped because another area is currently blocking.

## 4. Evidence and status rules

- Source/build presence => inventory evidence only.
- Host test => logic evidence only, not a hardware driver proof.
- x86-64 build => x86-64 compile evidence only.
- ARM64 build => ARM64 compile evidence only.
- QEMU => virtual hardware evidence only.
- Limbo => Limbo-specific x86 virtual hardware evidence only.
- Physical hardware => only the named, tested device/firmware configuration.
- A PR's CI result applies only to its exact tested SHA. It does not automatically transfer to main or dependent PRs.
- Unrun or unavailable checks remain UNVERIFIED.
- A release image must be checked against an explicit profile manifest; a successful ISO build alone does not prove all required components are present.

## 5. Next source-audit queue

1. Finish exact-head validation of PR #152 (F-001) and close any CI failures without weakening tests.
2. Complete platform handoff and early boot audit on x86-64 and ARM64.
3. Audit IRQ/timer/SMP/TLB operations and per-CPU state.
4. Audit PMM/VMM page-table ownership, reserved-page semantics, atomic block release on x86/ARM64, process address-space lifecycle, user-access boundaries and MMIO arithmetic.
5. Audit Device Model/PCI topology and DMA/MSI security before enabling device DMA.
6. Reconcile source inventory with build scripts and PR #148 file-loss manifest.
7. Produce a complete file-to-component-to-build-profile matrix for all 829 baseline blobs.
