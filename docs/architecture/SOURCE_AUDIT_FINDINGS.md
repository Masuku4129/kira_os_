# KIRA OS — подтверждённые первичные source-audit findings

**Кодовая база:** `main` @ `bbc7f7ab948e72511aed113b69043add10ec8350`  
**Дата:** 2026-11-12  
**Метод:** чтение исходников через GitHub; тесты в этой сессии не запускались.  
**Статус:** findings требуют независимого воспроизведения и/или проверки текущего PR; не являются окончательным security verdict.

## F-001 — kernel stack slot allocator не защищён от повторного освобождения

- **Severity:** High — memory corruption / duplicate allocation risk.
- **Files:** `vmm.c`, функции `vmm_kstack_alloc`, `vmm_kstack_free`.
- **Evidence:** allocator возвращает слот из `ks_free[--ks_nfree]`; free проверяет только диапазон адреса, затем без проверки ownership/active state делает `ks_free[ks_nfree++] = slot`. Нет проверки точного base, активного владения и capacity перед append. При double-free один slot может попасть в free list дважды и быть выдан двум потокам; повторные освобождения могут переполнить free list.
- **Related work:** открытый draft PR #132 `fix/vmm-kstack-double-free` добавляет active-slot bitmap, exact-base/ownership checks, bounded free-list append и shared helper `kstack_slot.h`. Его head SHA: `2b8569ad1baf8596e48c81ebebcdcefe8fae859d`; base — `v46/unified-release-integration`, не main.
- **Tests in PR:** `tests/kstack_slot_test.c` покрывает exact base, bounds, ownership, double-free, duplicate/corrupt free-list entries, full list and 65,536-slot ARM64 index boundary; подключён в host suite с ASan/UBSan. Но это тест helper, не полная интеграция actual VMM allocator, SMP concurrency, mapping failure or kernel stack reuse. PR body прямо говорит, что isolated allocator test отсутствует; поэтому эти тесты не доказывают весь allocator lifecycle.
- **Action:** не создавать конкурирующий fix поверх main. Проверить весь PR ancestry и внести helper+allocator fix в main-based integration branch после review; добавить тесты реального allocator lifecycle/concurrency, partial map failure, stack guard and reuse; затем x86-64/ARM64 builds и QEMU lifecycle stress. До этого помечать stack-slot lifecycle как unverified/high-risk.

## F-002 — KDRV publisher authenticity не подтверждена кодом main

- **Severity:** High — supply-chain trust boundary.
- **Files:** `kdrv_format.h`, `kdrv_fmt.c`, `kdrv.c`, `fsrv.c`, `docs/DRIVERS.md`.
- **Evidence:** текущий `kdrv_rec_t` содержит SHA-256 package/ELF hashes, source string, confirmation marker and granted capabilities. `kdrv.c:pkg_load` сверяет digest package и ELF с локальной записью `.rec`; `kdrv_fmt.c:kdrv_rec_check` проверяет структуру/confirmation/cap mask. В просмотренном main path не обнаружена криптографическая подпись издателя, проверка доверенного signer key или anti-rollback publisher version. SHA-256 обнаруживает несовпадение с локальной записью, но сам по себе не аутентифицирует издателя, если атакующий может заменить и пакет, и запись.
- **Documentation mismatch:** `docs/DRIVERS.md` описывает проверку подписи, но main implementation path, просмотренный в этом аудите, доказывает только hash/confirmation record. Это следует считать документально-кодовым расхождением до интеграции/проверки PR.
- **Related work:** PR #41 adds an Ed25519 package signature verifier, embedded public-key trust list, key ID, validity interval, package digest/size binding and install-time signature handling; PR #42 adds publisher-required policy when a package record says publisher signing is mandatory. These changes are not in main. #42 also carries a very broad cumulative diff (95 files, including boot, IOMMU, KDRV, tests and release tooling), so it must not be treated as a narrow signature-only change. ADR-0002 in that chain explicitly states strong anti-rollback is unavailable without a hardware-backed monotonic store; ordinary KiraFS records/generations are insufficient. The signature and anti-rollback controls must remain distinct.
- **Action:** review the full PR ancestry and exact cumulative diff; verify trust anchor provisioning, key rotation/revocation, signer identity, signed content domain (manifest + ELF + permissions + device match), expiry/time trust, anti-rollback limits and offline recovery. Add tamper tests replacing both package and .rec, missing signature for publisher-required packages, invalid/unknown/revoked signer, expired signature, key rotation and downgrade with disk rollback; require fail-closed.

## F-003 — Linux/Windows compatibility runtimes absent from main source map

- **Severity:** Architecture blocker, not an immediate defect.
- **Evidence:** native `elf.c` path is KIRA's own ELF64 executable format/personality (machine selected by build target); no dedicated Linux syscall personality, Linux dynamic-linker profile, PE/COFF loader or Win32/NT user-mode dispatch identified in the inspected main tree.
- **Action:** implement as separate user-space runtimes against the shared capability-mediated host API. Track actual application-level vertical slices, not parser-only milestones. Specs are now in `docs/compat/`.

## F-004 — Hardware hypervisor not established by the existing VMM

- **Severity:** Architecture blocker, not an immediate defect.
- **Evidence:** `vmm.c` implements host virtual address mapping/page tables and kernel stack slots. It is not evidence of VMX/SVM/EL2 guest execution, vCPU lifecycle, VM exits, EPT/NPT/Stage-2, virtual devices or guest isolation.
- **Action:** separate hypervisor control plane and architecture backends; threat model and acceptance criteria in `docs/virtualization/HYPERVISOR_ARCHITECTURE.md`.

## F-005 — Capability authority is currently static/name-based

- **Severity:** Medium/High — depends on threat model and trust of launch paths.
- **Files:** `caps.c`, `proc.c`, `syscall.c`.
- **Evidence:** `caps_for_program(name)` maps a fixed application name to a bitmask; `proc.c:spawn` assigns that mask at launch. `syscall.c:cap_needed` checks syscall capability bits centrally. No general object-bound capability handle, delegation/revocation API or dynamic policy authority was identified in the reviewed files.
- **Action:** preserve the existing deny-by-default allowlist as baseline while specifying object capabilities, endpoint rights, delegation/revocation and audit. Do not weaken current syscall checks during migration.

## F-006 — Mind action surface is direct and needs a formal policy gate

- **Severity:** Medium/High — privileged policy boundary.
- **Files:** `mind_sys.c`, `mind.c`, scheduler/process/resource APIs.
- **Evidence:** action callbacks include scheduler quantum/weight/limit, process heap restriction/kill, USB HID verdict, display effects and CPU reservation. The Mind policy is not entirely unbounded: most actions run only in `MIND_ACT`; scheduler tuning uses measured 8-sample trials and rollback; memory actions have staged 88%/93%/97% thresholds, a 10-second kill interval, and kill only when the largest process heap is at least 16 MiB and at least 20% of total RAM. However, callbacks directly invoke subsystem functions; a common typed request→owner policy→apply→audit/rollback contract is not present in the reviewed path.
- **Action:** preserve current safeguards; document and test every threshold/rollback. Add resource-owner validation, action reason/bounds/TTL and audit IDs to the control boundary. Require explicit user policy for destructive kill behavior and test false-positive scenarios. Move noncritical policy out of kernel where feasible; keep final enforcement at resource owners.

## F-007 — VFS/KiraFS concurrency evidence differs between host tests and production

- **Severity:** Medium — test validity concern.
- **Files:** `vfs.c`, `kfs.c`.
- **Evidence:** `vfs.c` deliberately compiles lock operations as no-ops under `KIRA_HOST_TEST`; production path uses kmutex. `kfs.c` similarly omits lock in `KIRA_HOST_TEST`/`KFS_NO_THREADS`. Host tests therefore cannot prove lock correctness, races or production concurrency behavior.
- **Action:** keep host tests for format/logic correctness, but add kernel/QEMU concurrency tests with real mutexes and fault injection. Explicitly document what each host test does not cover.

## F-008 — WASM production runtime has a deliberate time-slice, but fuel accounting is host-test-only

- **Severity:** Needs validation; not classified as a defect from this evidence alone.
- **Files:** `userland/wasm.c`.
- **Evidence:** `WASM_FUEL` enables deterministic fuel exhaustion accounting in host tests; production build compiles `FUEL_TICK` to no-op and uses periodic `wasm_slice()` yielding based on elapsed time. This is a valid alternative only if process cancellation, scheduler fairness and non-yielding paths are proven.
- **Action:** add/verify a production runtime test for infinite loops, tight loops, host-call loops, cancellation and scheduler responsiveness on x86-64 and ARM64. Do not equate host-test fuel exhaustion with production instruction quota.

## F-009 — broad syscall dispatcher is a cross-layer coupling hotspot

- **Severity:** Architecture debt.
- **Files:** `syscall.c`, `fsrv.c`, `gfxsrv.c`, `net.c`, `audio.c`, `kdrv.c`.
- **Evidence:** `syscall.c` includes and dispatches directly to many service implementations; syscall numbers cover graphics, files, IPC, network, audio, drivers and JIT. Some are legitimate kernel primitives; others appear to be service calls.
- **Action:** preserve stable syscall ABI while moving service-level behavior behind capability-checked IPC/service endpoints. Do not break user ABI or existing apps during staged migration.

## Validation state

No tests were executed during this source-review pass. Findings above are code observations and risk statements, not test failures. The next validation sequence is:
1. review PR #132 and reproduce allocator ownership invariants;
2. review KDRV signature PR chain and test replacement of both package and record;
3. run existing host tests and production x86-64/ARM64 builds at a recorded SHA;
4. add production/QEMU tests for VFS/KFS concurrency, WASM scheduling, Mind control and driver lifecycle;
5. update each finding with fixed commit, test command, exact result and residual risk.


## F-010 — IPC receive can consume a message before user-buffer copy succeeds

- **Severity:** Medium — documented semantic edge case, not automatically a defect.
- **Files:** `syscall.c` SYS_PORT_RECV, `kobj.c:port_recv`, `uaccess.c`.
- **Evidence:** `port_recv()` dequeues the message and copies into a kernel buffer; after it returns, SYS_PORT_RECV copies the bytes to user memory. If `copy_to_user()` fails because the destination was concurrently unmapped or became invalid, the message has already been removed and is lost; the source comment explicitly records this behavior.
- **Action:** decide and document whether IPC receive is at-most-once with loss on invalid destination. If not acceptable, design a retry-safe protocol (pre-pin destination pages, dequeue token/ack, or other ownership protocol) without touching user memory under a spinlock. Add race/fault tests. RFC-IPC-001 tracks this open decision.



## F-011 — KDRV catalogue/trust/manager proposal in PR #42 is not wired into production runtime

- **Severity:** High — integration/assurance gap; not a claim that the standalone library logic is incorrect.
- **Evidence:** In PR #42 branch `v46/kdrv-catalogue-signature-policy`, new `drivers/kdrv/kdrv_trust.c`, `kdrv_catalogue.c` and `kdrv_manager.c` are compiled by `tests/run_host_tests.sh`, but the production `build_clang.sh` source list and kernel entry paths do not compile/call these modules. `kernel.c` and `arch/aarch64/main.c` do not reference `kdrv_catalogue_load`, `kdrv_manager_install` or `kdrv_trust_verify_catalogue`. The current production driver path remains the existing `kdrv.c` package loader; PR #42 also modifies that path for detached per-package Ed25519 signatures and publisher-required .rec policy.
- **Trust provider gap:** `kdrv_trust_ops` delegates Ed25519, SHA-256, trusted time and monotonic version storage to callbacks. The PR's catalogue test supplies mock hash/verify/time/floor callbacks and sets `KDRV_TRUST_STORAGE_HW_MONOTONIC`; ADR-0002 explicitly says no real hardware-backed monotonic provider exists. The API flag is a contract assertion, not proof that a platform provider exists.
- **Action:** separate the production per-package signature path from the new signed-catalogue architecture. Before calling catalogue trust production-ready, wire a real key store, trusted-time source, cryptographic provider, hardware monotonic storage provider, serialized catalogue→package activation path and persistent rollback/recovery behavior. Keep host mock tests labeled policy-only; require platform evidence for the monotonic flag.

## F-012 — PR #42 introduces two incompatible BootInfo v1 structures, one of them unintegrated

- **Severity:** High — architecture/ABI duplication risk.
- **Evidence:** PR #42 branch contains root `bootinfo.h` (`kbi_header_t`, 136-byte packed header, magic `0x4941524B`, offset-based blob with x86/ARM architecture, memory map, modules, cmdline, framebuffer, ACPI/FDT) which is consumed by `kernel.c`, `arch/aarch64/main.c`, `bootinfo.c`, `bootinfo_mb2.c` and `bootinfo_fdt.c`, and compiled by build scripts. The same branch also adds `include/kira/bootinfo.h` with a different 64-bit magic, different struct fields and descriptor-pointer model, plus `kernel/boot/boot_handoff.[ch]` using that second format. The latter handoff source is not in the production build source lists and its entry function is not called by the kernel entry paths reviewed.
- **Action:** do not integrate both formats. Keep one canonical KIRA BootInfo ABI, one version/magic/layout, one validator and architecture-specific adapters. Remove or explicitly quarantine the unused alternate format only after confirming no dependent PR consumes it. Add a cross-ISA fixture and compile-time ABI size/offset assertions to the canonical contract.



## F-013 — main enables PCI bus mastering for Ring 3 drivers without IOMMU isolation

- **Severity:** Critical/High — device DMA can corrupt or disclose arbitrary host physical memory.
- **Files:** `kdrv.c:start()`, `kdrv.c:kdrv_sys()`, `kdrv.h`, `caps.h`.
- **Evidence on exact main baseline:** after `granted = p.h.caps & p.r.caps_ok`, the start path rejects CAP_DMA only when `platform_dma_noncoherent()` reports a non-coherent platform. It does not require an IOMMU domain. It then enables `PCI_CMD_MASTER` when either `CAP_DMA` **or `CAP_IRQ`** is granted. Thus a user-confirmed Ring 3 driver with DMA or IRQ capability can enable bus mastering even though the main KDRV contract explicitly states that without IOMMU a device may write any physical memory. CAP_IRQ also enables bus mastering for MSI message writes, so treating only CAP_DMA as dangerous would be insufficient.
- **Related work:** PR #42 branch adds `kdrv_dma_policy_allow(granted, 0)` and fail-closed rejection because no per-BDF VT-d/AMD-Vi/SMMU domain exists. This policy is not present in main. PR #125/#120/#116/#115 and #44 address IOMMU foundations, but the complete domain lifecycle and hardware isolation remain unproven.
- **Action/status:** a narrow main-based fix is open as PR #136 (`security/kdrv-require-iommu-for-dma-msi`, head `720a2020f6d78501aed60b5013ad9b7d1c62b30d`). It adds a host-testable policy, rejects either CAP_DMA or CAP_IRQ when no domain is confirmed, quiesces the device before rejection, adds a distinct error and tests the capability matrix. PR is not merged. Latest Actions runs for the preceding code/docs commits were pending/in progress when checked; final-head verification must be refreshed. Review that rejection occurs before any bus-master enable, MSI/MSI-X setup or process spawn. Then test device removal, domain teardown and fault handling. Do not claim DMA isolation based on reserved DMA buffers alone.



## F-014 — PCI enumeration preserves firmware-enabled bus mastering

- **Severity:** High — DMA can remain active before KIRA assigns ownership or applies driver policy.
- **Files:** `pci.c:scan_fn()`, `pci.c:probe_bars()`.
- **Evidence:** `scan_fn()` reads identity/class/header and calls `probe_bars()` without clearing `PCI_CMD_MASTER`. `probe_bars()` reads the command word, temporarily clears only `PCI_CMD_IO | PCI_CMD_MEM`, then restores the full original command word. If firmware hands off a function with bus mastering enabled, that bit survives enumeration and may remain active before any KIRA driver has claimed the device.
- **Action/status:** narrow fix is open as PR #137 (`security/pci-clear-firmware-busmaster`, current head to be refreshed). It clears `PCI_CMD_MASTER` for each discovered function before BAR probing, preserving IO/MEM bits; built-in drivers can explicitly enable it after ownership. Adds a fake PCI backend regression test and wires it into host tests. PR not merged; exact-head CI is pending. This does not replace PR #136: firmware handoff sanitation and KDRV per-device IOMMU gating are both required.
