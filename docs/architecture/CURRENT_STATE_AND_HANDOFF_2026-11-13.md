# KIRA OS — current architecture state and continuation handoff

**Snapshot date:** 2026-11-13  
**Repository:** `Masuku4129/kira_os_`  
**Working branch:** `architecture/target-baseline-2026-11`  
**Open architecture PR:** [#135](https://github.com/Masuku4129/kira_os_/pull/135)  
**Initial source-review head:** `8626c4e1f063796da383880e656a36d654fb27fa` (documentation commits were added after this review; refresh the live PR head before work)  
**Base:** `main` at `bbc7f7ab948e72511aed113b69043add10ec8350`  
**Status:** working architecture baseline; PR remains Draft; no merge authorized.

This document is the dated continuation pointer. The normative design remains in the master plan and target architecture. If this snapshot conflicts with live GitHub metadata, live branch/PR/CI state wins; if it conflicts with a normative architecture/RFC, record and resolve the discrepancy rather than silently changing the design.

## 1. Product direction

KIRA OS is an independent operating system with its own kernel, system ABI, security authority, services, runtimes, user environment and intelligent management subsystem. It is not a Linux distribution, a desktop shell over another OS, or merely a hypervisor front end.

The selected direction is **`independent_all`**; the current engineering priority is **architecture**. x86-64 and ARM64 are first-class target ISAs. Physical hardware support is not claimed until hardware-specific boot, driver, DMA, power and recovery paths have been tested on representative devices.

## 2. Eight logical levels

These are responsibility boundaries, not a requirement that each level already be a separate process, binary, or source directory.

| Level | Name | Owns |
|---|---|---|
| 1 | Hardware / Platform | CPU, RAM, firmware, buses, physical devices and platform description |
| 2 | HAL / Device Model / Drivers | ISA backends, device discovery, resources, interrupts, DMA, driver lifecycle and hardware adapters |
| 3 | KIRA Kernel | privileged execution, address spaces, memory, threads, scheduler, syscall entry, IPC primitives and synchronization |
| 4 | Security | identity, capabilities, policy decisions, trust, signatures, sandbox boundaries and audit |
| 5 | KIRA Mind | telemetry interpretation, diagnostics, forecasting and bounded action proposals |
| 6 | System Services | storage/VFS, network, graphics, audio, package/update, service lifecycle, power and observability |
| 7 | Runtime Environment | native runtime, WASM/KVR baseline, experimental KIRA VM, Linux and Windows application compatibility |
| 8 | User Environment | desktop, application shell, settings, explorer, terminal and user-facing system tools |

**Separate subsystem:** KIRA Hypervisor provides whole-guest-OS virtualization (VM/vCPU, guest memory, VM exits, virtual devices, interrupt/timer virtualization, snapshots and teardown). It is not the KIRA VM/WASM runtime and not the Windows/Linux application compatibility layer.

**Cross-cutting systems:** Boot Manager/Installer/Update/Recovery, KiraFS/VFS, KCC/toolchain, Package Manager, Driver Manager, Service Manager, Event Bus, Resource/Power Manager, Observability and crash recovery. Each must have a named owner, contract, privilege boundary and tests.

## 3. Non-negotiable architecture rules

1. Kernel remains the final authority for privileged operations. KIRA Mind may observe and propose; it cannot self-grant rights or bypass policy.
2. Use least privilege, fail-closed validation, explicit capabilities and auditable authority transitions.
3. Keep architecture-specific mechanisms behind HAL/backend contracts; preserve common semantics across x86-64 and ARM64.
4. Separate source ownership from privilege/process isolation. A file under `services/` or a syscall adapter does not prove that a service is in user mode.
5. Distinguish Native, WASM/KVR, experimental KIRA VM, Linux compatibility, Windows compatibility and whole-OS Hypervisor execution. Do not conflate their guarantees.
6. Preserve the existing working Limbo/QEMU paths while migrating one dependency-closed slice at a time.
7. No empty functions, fake production logic, mock-only claims, or unsupported hardware claims.
8. Every claim of PASS must identify the exact commit, test/job, and observed result. A source-level check is not a runtime test.
9. Never merge PRs or change `main` without explicit owner authorization.

## 4. Existing architecture documentation map

Read these documents before implementation; they are complementary, not alternatives:

- `docs/MASTER_DEVELOPMENT_PLAN.md` — full product scope and all parallel development streams.
- `docs/architecture/KIRA_OS_TARGET_ARCHITECTURE.md` — normative eight-level target, subsystem boundaries and contracts.
- `docs/architecture/ARCHITECTURE_AUDIT_AND_EXECUTION_PLAN.md` — ordered audit and execution gates.
- `docs/architecture/SOURCE_LAYOUT_MIGRATION.md` — current source ownership and target layout.
- `docs/architecture/COMPONENT_INVENTORY.md` — component existence and implementation status.
- `docs/architecture/DEPENDENCY_MAP.md` — dependency and initialization relationships.
- `docs/architecture/CONTRACT_REGISTRY.md` — contract owner, status and acceptance criteria.
- `docs/architecture/SOURCE_AUDIT_FINDINGS.md` — evidence-backed findings and risks.
- `docs/architecture/OPEN_PR_ARCHITECTURE_TRACKER.md` — branch/PR dependencies and integration warnings.
- `docs/architecture/rfc/` — draft contracts, including `RFC-DEV-001.md` for Device Model; a Draft RFC is not a frozen ABI.
- `docs/compat/` — Linux/Windows application compatibility and its test matrix.
- `docs/virtualization/HYPERVISOR_ARCHITECTURE.md` — whole-guest virtualization.
- `docs/WORKLOG.md` — chronological work and exact continuation point.
- `AGENTS.md` and `README.md` — mandatory agent workflow and entry point.

## 5. Current source-level progress on PR #135

The current branch contains physical source ownership slices, including:

- x86-64 boot composition and kernel main-loop ownership moved under `arch/x86_64/`; ARM64 entry and steady-state loop ownership under `arch/aarch64/`.
- Syscall core at `kernel/syscall.c`; subsystem adapters/router under `services/`.
- Graphics, filesystem, network, audio, clock and Driver Manager syscall adapters are separated from the central syscall core.
- Canonical syscall-number registry is `kcc/include/syscall_numbers.h`; numeric IDs are intended to remain unchanged.
- Architecture guards check source ownership, syscall-ID ownership, capability gates and both ISA build manifests.
- ARM64 FDT PCI host bridge/ECAM parsing and BAR-window setup have been extracted into `arch/aarch64/pci_host.c` and `arch/aarch64/pci_host_parse.c`.

These are source-organization and boundary improvements. In particular, service adapters still execute in kernel context; separate user-mode service isolation and IPC-based service lifecycle are not yet established by this split.

## 6. Verification evidence and limits at this snapshot

- The source-review head at the start of this documentation pass was `8626c4e1f063796da383880e656a36d654fb27fa`; documentation-only commits followed. Read the live PR #135 page for the current branch head before every new task.
- The latest fully successful recorded architecture CI run is [43298087855](https://github.com/Masuku4129/kira_os_/actions/runs/43298087855), which tested commit `618ce01344e219991323aca51f9de58c55ed2aab`: 30/30 jobs passed, including host tests, x86-64 build, ARM64 build, and the QEMU/Limbo VM matrix (including ARM64 GIC/EL2 coverage).
- That successful run predates the later PCI host follow-up changes now present at the snapshot head. It does **not** verify `8626c4e1...`.
- Draft PR #139 adds `pci_init_range(first_bus, last_bus)`, bounds recursive bridge traversal and makes ARM64 consume the validated FDT bus-range, including nonzero starts. It now also adds boot-local monotonic PCI IDs, a scan generation, and opaque snapshot handles (`pci_get_handle`/`pci_resolve`) that reject stale handles after a rescan. This is still a PCI-only migration seam, not a generic Device Model registry. KDRV binding identity now uses `pci_handle_t`; every device operation resolves through `pci_resolve()`, and a stale handle quarantines the binding while retaining DMA resources if quiesce cannot be confirmed. A static architecture guard enforces the migration. Exact-head CI for this KDRV slice is pending.
- Therefore the source-review head is **UNVERIFIED**. Documentation-only commits do not change that code-verification status; do not carry the 30/30 result forward. A fresh exact-head workflow is still required.
- No physical x86-64 or ARM64 hardware validation is recorded here. QEMU/Limbo evidence must not be represented as physical-device validation.

## 7. Immediate execution queue

### Gate 0 — exact-head integration verification
1. Run the complete CI matrix on the current PR #135 head.
2. Verify x86-64 build, ARM64 build, architecture guards, full host suite, KCC, and all QEMU scenarios.
3. If a job fails, fix the root cause on the same architecture branch and rerun the full exact-head matrix.
4. Record the exact SHA and individual job outcomes in `docs/WORKLOG.md`; do not report a blanket PASS based on a different SHA.

### M2 — HAL / Device Model / BootInfo contracts
1. Verify PR #139's exact-head host tests, x86-64/ARM64 builds and QEMU bridge traversal. Keep recursive enumeration fail-closed outside the declared [start,end] range.
2. Extend PCI contract tests to ECAM address translation, multifunction devices, cycles, duplicate devices and malformed config responses; then implement common resource ownership and coordinated rescan exclusion; KDRV binding identity has migrated to generation-safe PCI handles, with stale bindings quarantined.
3. Continue the unified BootInfo RFC from actual x86 Multiboot2 and ARM64 FDT paths. Resolve duplicate candidate structures in dependent branches before freezing any layout.
4. Define Device Model identity and lifecycle: discovery → resource validation/reservation → driver match → capability grant → start → quiesce/stop → revoke/release → remove/recovery.
5. DMA/MSI must remain blocked unless a real per-device IOMMU domain is attached, validated and torn down safely. Reconcile the independent security PRs before PCI bus mastering is enabled by any driver.
6. Update component inventory, dependency map, contract registry, architecture guards and both ISA manifests with each accepted source slice.

### M3 — security authority
- Map legacy process capability bits to object-scoped handles/rights.
- Specify issuer, recipient, object generation, delegation, revocation, inheritance, expiry and audit semantics.
- Test stale handles, cross-process confusion, revoke races, driver removal and Mind authority boundaries.
- Preserve current fail-closed behavior during migration; no broad ABI change without a versioned migration plan.

### Parallel workstreams remain active
Continue all master-plan streams: kernel/memory/SMP, KVR and application runtimes, Mind, KCC/toolchain, memory safety, drivers/IOMMU, storage/filesystems, networking, graphics/audio/input, security/trust, installer/update/recovery, and compatibility. Do not let the immediate PCI/CI gate erase or silently defer the rest of the project scope.

## 8. Next session checklist

- [ ] Re-read this snapshot, `AGENTS.md`, full master plan, target architecture and current worklog.
- [ ] Refresh live PR #135 head and CI before coding.
- [ ] Confirm the current head SHA; do not assume it remains `8626c4e1...`.
- [ ] Close exact-head CI gate.
- [ ] Continue with the smallest dependency-closed M2 contract/source slice.
- [ ] Update source inventory, dependency map, contract registry, tests and worklog.
- [ ] Leave PR #135 Draft and unmerged pending owner review.
