# KIRA OS — Stage 0 test and verification matrix

**Pinned implementation baseline:** main `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`  
**Purpose:** map existing tests to components, separate cheap diagnostics from acceptance, and expose untested contracts.  
**Important:** this is a test inventory and plan, not a statement that these tests were executed in this audit session.

## 1. Test layers — run in this order

| Layer | When to run | Scope | Expected cost | Acceptance rule |
|---|---|---|---|---|
| T0 — static/source checks | Every change | exact changed files, API/ABI, lock order, build inclusion, branch overlap | seconds | review evidence and clean diff |
| T1 — focused host test | Every code change | one relevant test binary, ASan/UBSan where supported | seconds–minutes | exact test command and zero exit |
| T2 — focused QEMU diagnostic | Every boot/device regression | one scenario, short boot timeout, serial markers/register snapshots | target 10–15 s | failure localized; no giant log by default |
| T3 — subsystem acceptance | After focused fix | full related host tests + existing QEMU scenario(s) | minutes | all related positive and negative paths pass |
| T4 — cross-architecture | Any shared kernel/ABI/driver change | x86-64 build + ARM64 build and matching QEMU cases | minutes | both ISA contracts pass; hardware caveat recorded |
| T5 — full exact-head CI | Before PR acceptance | host tests, x86 build, ARM64 VM, all QEMU scenarios, audits | longest | all jobs pass on the exact final SHA |
| T6 — integration compatibility | Before combining Draft PRs | changed-file overlap, dependency order, full merged-tree CI | longest | no duplicate/reverted changes; one coherent stack |

**Do not** start T5 after every small edit. Do not replace T5 with a fast diagnostic. Do not accept a fix because its parent or earlier PR head passed.

## 2. Existing host-test coverage from tests/run_host_tests.sh

| Component | Existing host tests / commands | Current coverage boundary / missing checks |
|---|---|---|
| Hashing, self-integrity, package trust | `tests/logic_test.c`, `tests/kdrv_test.c`, `tests/drvget_test.c`, `tests/crypto_test.c` | Add end-to-end signed-driver trust chain and key-rotation/revocation acceptance when signature/state integration lands |
| Heap and physical memory | `heap_test.c` (ASan/UBSan), `pmm_test.c`, `ramext_test.c`, `uheap_test.c` | Current-main PMM/VMM cross-ISA invariants need more direct guard/reservation/TLB negative tests; branch-only PMM fixes must be integrated coherently |
| Scheduler, context, FPU, SMP | `sched_test.c`, `ctx_fpu_test.c` (real FXSAVE/FXRSTOR and no-FPU), `kmutex_test.c`, `smp_test.c`, `scale_test.c`, `spinlock_test.c` (TSan) | Add dedicated QEMU guarded-stack exhaustion and guard-page fault tests on both ISAs; exercise affinity migration and teardown races |
| Input and graphics primitives | `input_test.c`, `gfx_test.c`, `ui_test.c`, `wm_test.c`, `display_preview.c`, `fm_test.c`, `dock_test.c` | Full GUI/Spatial acceptance still needs exact visual target and real interaction evidence; host pixel tests do not prove display hardware |
| PCI and device model | PCI is exercised through `virtio_test.c` with `pci.c`; KDRV package and RTL8139 model tests also exist | No separate broad PCI config-space/bridge/resource ownership suite in the current host runner; add command-register ordering and multi-function/bridge adversarial cases |
| VirtIO and block storage | `virtio_test.c` links `virtio.c`, `virtio_blk.c`, `pci.c`, KiraFS | Add firmware-active/stale-queue DMA tests and command-register readback/fail-closed tests |
| USB stack and controllers | `usbhid_test.c`, `usb_test.c`, `uhci_test.c`, `xhci_test.c` with controller models | Add firmware-started controller state, invalid prior frame-list/ring bases, stop/reset timeout, and Bus Master denied-before-ready tests |
| Storage/filesystems | `store_test.c`, `kfs_test.c` (crash at each write + corruption fuzz), `fat_test.c` (conditional on dosfstools/mtools) | FAT fake device currently has no flush callback; no test catches discarded flush failure. Add concurrent VFS mount/unmount snapshot tests |
| Programs and user ABI | `prog_test.c`, `fsrv_test.c`, `paint_test.c`, `kobj_test.c`, `userlog_test.c`, `corefault_test.c` | Add full multi-thread process teardown/handle-close races and service criticality / termination-veto tests |
| Network and TLS | `net_test.c`, `ipfrag_test.c`, `ip6_test.c`, X.509/TLS OpenSSL interop, `drvget_e2e.sh` | Need physical NIC matrix, network-device teardown under traffic, and IOMMU-safe DMA tests |
| KIRA Mind | `mind_test.c`, `mind_lever_test.c`, `mindnet_test.c` | Add protected-service, unsaved-work, foreground, driver-process and restartability kill-veto tests; current host tests do not establish general AI reasoning |
| Runtime/toolchain | WASM differential/fuzz/JIT tests; `tests/kcc/run.sh`; native KCC suite | Add native ABI/dynamic-linking and Linux/Windows compatibility conformance; these remain planned |
| Audio | `audio_test.c`, `sb16_test.c` | Add firmware-active DMA/ring ownership and hardware output/capture acceptance |

### Host-runner caveats

- FAT tests run only when `mkfs.fat`, `fsck.fat` and `mcopy` are installed. A skipped test is not a pass.
- Several host tests deliberately use test models or fake kernel APIs. These are valid unit-test boundaries, not production implementations.
- `tests/run_host_tests.sh` has broad coverage, but a single green host run cannot replace QEMU device and boot scenarios.

## 3. Existing x86-64 QEMU scenario map

| Scenario | Main purpose | Triggered subsystems |
|---|---|---|
| `boot-virtio`, `boot-ata`, `boot-sata`, `boot-nvme` | boot and system disk paths | boot, PCI, VirtIO/ATA/AHCI/NVMe, KiraFS |
| `boot-1cpu`, `boot-8cpu`, `soak-8cpu`, `smp` | CPU count, scheduling, long-running stability | HAL, timer, scheduler, SMP, memory |
| `isolation`, `crash-isolation` | address spaces, caps, IPC, threads, FPU, fault containment | processes, syscalls, VMM, IPC, core fault policy |
| `usb-uhci`, `usb-xhci` | USB HID, hubs, flash drive and FAT VFS | PCI, DMA, UHCI/xHCI, USB stack, FAT, VFS |
| `net-e1000`, `net-e1000e`, `net-virtio`, `net-ipv6` | DHCP, ping, TCP/UDP, IPv6, driver network path | PCI, DMA, NIC drivers, network stack |
| `sound-hda`, `sound-ac97`, `sound-sb16` | output/capture through device models | PCI/ISA, DMA, audio |
| `stress-spam`, `stress-spam-2`, `stress-spam-4`, `stress-3d`, `limbo-spam`, `limbo-mttcg` | contention, UI response and emulator modes | scheduler, Mind, graphics, memory |
| `apps-edit`, `apps-explorer2`, `apps-view`, `apps-wasm`, `wasm-speed` | application workflows and runtime | GUI, file service, KiraFS, WASM |
| `power-off`, `power-reboot` | ACPI power transitions and persistence | ACPI, storage flush, boot |
| `mind-levers`, `mind-seed`, `mind-train` | adaptive policies, saved experience, training run | KIRA Mind, KiraFS, scheduler |
| `cc-native`, `drv-edu`, `drvget-rl`, `drvman-edu`, `apps-drvman` | compiler and Ring 3 driver workflows | KCC, KDRV, trust, PCI, DMA, driver UI |

## 4. Existing ARM64 QEMU coverage

The ARM64 runner includes boot/firmware, GICv2/GICv3, 1–8 CPU SMP, isolation, capability checks, FPU, threads, WASM interpreter, crash isolation, stack guard and kernel fault scenarios. The ARM64 path is QEMU TCG coverage only; it is not physical-board acceptance.

For each shared contract change, choose the ARM64 scenario that actually exercises that contract. A successful ARM64 build alone does not prove the runtime path.

## 5. Targeted tests required by current findings

| Finding | Fast test to add/run first | Focused QEMU reproduction | Full acceptance after fix |
|---|---|---|---|
| F-017 / F-025 PCI enumeration + UHCI hang | PCI Command read/write model; assert markers and command value | `usb-uhci` with short diagnostic timeout and separate read/write markers | full `usb-uhci`, `usb-xhci`, all PCI device scenarios, x86 + ARM64 CI |
| F-029 / F-030 driver DMA ordering | fake PCI device asserts Bus Master stays clear through reset and ring programming | firmware-started UHCI/xHCI/VirtIO/NVMe/AHCI/e1000/HDA/AC97 with stale DMA pointers | all relevant storage, USB, network, sound scenarios; IOMMU and fail-closed matrix |
| F-031 KDRV capability grant | CAP_IRQ-only, CAP_DMA startup failure, spawn failure, teardown/readback tests | Ring 3 edu and RTL8139 drivers with invalid/unregistered DMA requests | driver e2e, drvman, drvget, isolation, x86 + ARM64 |
| F-032 Mind kill policy | table-driven victim selection with essential/foreground/unsaved/driver/restartable flags | memory pressure scenario with protected system service and foreground app | mind-levers, mind-train, stress and crash-isolation |
| F-033 FAT flush propagation | fake device with write and flush fault injection | USB flash write then failed flush/eject; ensure error is visible | usb-uhci/xhci + FAT fsck/remount + power transition |
| F-034 VFS mount table race | threaded mount/unmount vs vfs_any/root listing with slot reuse | USB mount/unmount under concurrent root listing | USB flash scenario, stress and full host tests |

## 6. QEMU diagnostic cost control

Current `ci/kira_vm.py` enables QEMU `-d guest_errors` and writes `qemu.log`. This is useful for a focused hardware fault investigation but can produce multi-gigabyte logs: the current UHCI run generated 4,187,160,076 bytes in 90.9 seconds.

Planned harness change (not implemented yet):
- add an opt-in `--guest-errors` switch;
- add a diagnostic profile with a 10–15 second boot timeout and concise serial/register assertions;
- default routine diagnostics to no full guest-error log;
- keep the existing 90-second acceptance scenario and its complete evidence unchanged;
- when a guest-error trace is needed, run one isolated scenario and stream/aggregate the log rather than loading it into memory.

## 7. Integration and concurrency rules

- Batch independent host-test builds and source reads; do not run duplicate full CI workflows for the same commit.
- Keep tests deterministic and bounded. Every timeout must identify the last completed checkpoint.
- When two Draft PRs touch the same file, compare exact patches and build one coherent integration stack before full CI.
- Test shared kernel contracts on both ISAs; do not assume identical initialization order.
- Keep prior failed runs as evidence. A later pass supersedes acceptance only for its exact SHA.
- No ISO build or release packaging until Stage 0 audit and the release gates are complete.
