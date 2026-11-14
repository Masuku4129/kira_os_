# KIRA OS — Stage 0 source audit findings (2026-11-14)

**Audit baseline:** `main` @ `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`  
**Status:** CONFIRMED SOURCE FINDINGS — runtime reproduction and regression tests are still required.  
**Parent record:** [Stage 0 repository audit](STAGE0_REPOSITORY_AUDIT_2026-11-14.md)

A source finding is not automatically a runtime failure. The findings below are confirmed by the source at the pinned SHA; test evidence is listed separately and is not implied.

## F-001 — Kernel stack allocator does not validate ownership or exact base

**Severity:** Critical memory-safety risk  
**Confidence:** High — direct source evidence in both ISA backends  
**Master-plan mapping:** B (kernel/process/thread lifecycle), F (memory safety), M (architecture parity), N (tests/quality); streams 1, 5, 11, 12.

### x86-64 evidence

In `vmm.c`, `vmm_kstack_free()` checks only that the supplied address is somewhere inside the kernel stack window, then unconditionally appends the derived slot to `ks_free[8192]` and increments `ks_nfree`. It does not require the exact base returned by the allocator, check that the slot was allocated, track active ownership, reject a duplicate free, or bound the append.

Source: [vmm.c at baseline SHA, lines 141–145](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/vmm.c#L141-L145).

### ARM64 evidence

In `arch/aarch64/main.c`, `vmm_kstack_free()` accepts any address inside the stack window and writes the free-list link directly into the supplied address. There is no exact-base or active-slot ownership check. This is a separate implementation of the same unsafe lifecycle.

Source: [arch/aarch64/main.c at baseline SHA, lines 353–361](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/arch/aarch64/main.c#L353-L361).

### Consequence

A duplicate or interior-pointer release can corrupt the free list and make the same kernel stack slot available more than once. Two kernel threads could then receive overlapping stacks; repeated releases can also exceed the free-list capacity. This is a kernel memory-safety issue, not merely an accounting defect.

### Related work and evidence limits

- Draft PR [#132](https://github.com/Masuku4129/kira_os_/pull/132) contains a shared slot-ownership helper, x86-64 and ARM64 changes, and a host test for exact-base, bounds, duplicate-free, reuse and ARM64 slot-index limits.
- PR #132 is based on `v46/unified-release-integration`, not current main. It must not be merged or copied wholesale without reconciling its full diff and dependencies.
- At this audit snapshot, PR #132 head `2b8569ad1baf8596e48c81ebebcdcefe8fae859d` has no check-runs. The PR itself says no isolated allocator host test previously existed; the added helper test is not a complete integration test of the production allocator.
- Current main `tests/run_host_tests.sh` has no kernel-stack slot lifecycle test.
- A separate main-based fix is now tracked in [PR #152](https://github.com/Masuku4129/kira_os_/pull/152). It adds shared exact-base/ownership helpers for both ISAs and a host regression test. At head `8358d14850ff48f0a710486106a550e01701f2a5`, workflow `43585911384` is in progress; no PASS is claimed.

### Additional x86-64 failure path

When a new stack mapping fails after one or more pages have already been mapped, `vmm_kstack_alloc()` returns without advancing `ks_used_hi`; already mapped pages remain. A later allocation retries the same virtual slot, but `map_in()` refuses to overwrite the existing PTE, so the slot can remain permanently unusable and its earlier pages remain allocated.

Source: [vmm.c at baseline SHA, lines 116–137](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/vmm.c#L116-L137).

**Required closure:** shared allocator-state logic for both ISAs; exact-base/active ownership; bounded free-list operations; safe partial-map failure handling; production allocator lifecycle tests including duplicate/interior/foreign frees, reuse, capacity, injected partial mapping failure and concurrent allocate/free; exact-head host, x86-64 and ARM64 CI. Do not claim closure from helper-only tests.

## F-002 — Ring 3 DMA/MSI can enable PCI bus mastering without per-device IOMMU isolation

**Severity:** Critical security boundary  
**Confidence:** High — direct source evidence and no IOMMU source module in main tree  
**Master-plan mapping:** A, B, F, G, M, N; streams 1, 5, 6, 11, 12.

### Evidence

- The current main tree contains no IOMMU/VT-d/SMMUv3 implementation files.
- In `kdrv.c`, driver startup rejects DMA only for the ARM64 non-coherent-pool case. It then enables `PCI_CMD_MASTER` whenever either `CAP_DMA` or `CAP_IRQ` is granted. MSI/MSI-X also depends on bus mastering.
- `sys_dma_alloc()` allocates contiguous physical pages and maps them into the Ring 3 driver; no per-device IOMMU domain is established in this path.
- ARM64 has a non-coherent DMA pool and GIC ITS/MSI allocation code. These are cache-coherency and interrupt-delivery mechanisms, not DMA address-space isolation.

Sources:
- [kdrv.c at baseline SHA, lines 206–224 and DMA allocation path](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/kdrv.c#L206-L224)
- [syscall.c at baseline SHA, capability gate](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/syscall.c#L119-L143)
- [ARM64 DMA pool and coherency selection](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/arch/aarch64/main.c#L165-L261)
- [ARM64 GIC ITS/MSI implementation](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/arch/aarch64/main.c#L460-L580)

### Consequence

A Ring 3 driver with DMA or interrupt rights can cause a bus-master-capable device to write to physical memory outside its assigned buffers if no hardware DMA isolation exists. A cache-incoherent DMA pool does not prevent arbitrary DMA.

### Related work and evidence limits

- [PR #136](https://github.com/Masuku4129/kira_os_/pull/136) proposes fail-closed denial of `CAP_DMA` and `CAP_IRQ` until per-device IOMMU isolation is confirmed. Its current head `761a212f3ab73c41f7a867c0337526b70cb8a174` has no check-runs.
- [PR #137](https://github.com/Masuku4129/kira_os_/pull/137) proposes clearing firmware-enabled PCI bus mastering before driver ownership. Its current head `d35c00069bf41bc0316a0dc743c7a9fe4a01be56` has no check-runs.
- These are separate protections: firmware handoff sanitization does not replace IOMMU isolation; a KDRV policy gate does not implement an IOMMU.
- Do not label DMA safe, isolated or production-ready until per-device domain setup, requester-ID mapping, attach/detach, invalidation, teardown, fault handling and adversarial tests are demonstrated on x86-64 and ARM64.

**Required closure:** first establish a fail-closed DMA/MSI policy; then implement and test real per-device IOMMU domain lifecycle for VT-d and SMMUv3 before allowing DMA/MSI. MMIO-only drivers remain a separate policy case.

## F-003 — PCI ECAM host bus range is stored but enumeration always starts at bus 0

**Severity:** High platform compatibility defect  
**Confidence:** High — direct source evidence  
**Master-plan mapping:** A, G, M, N; streams 5, 6, 11, 12.

### Evidence

- `pci_use_ecam()` records `bus_start` and `bus_end`.
- The ECAM backend returns `0xFFFFFFFF` for buses below `ecam_bus0`.
- `pci_init()` unconditionally calls `scan_bus(0)`; there is no range-aware root enumeration in current main.
- Therefore a valid ECAM host bridge whose first bus is nonzero is not enumerated correctly. Recursive bridge traversal also needs to be constrained to the host's declared range and each ancestor bridge's subordinate range.

Source: [pci.c at baseline SHA, lines 47–59 and 170–188](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/pci.c#L47-L59).

### Related work and evidence limits

- [PR #139](https://github.com/Masuku4129/kira_os_/pull/139) implements range-aware enumeration, bounded subordinate traversal and generation-safe handles, but it is stacked on PR #135 rather than main. Its current exact-head check-runs show 45/46 success with `vm (drv-edu)` failed.
- [PR #141](https://github.com/Masuku4129/kira_os_/pull/141) builds a common registry/PCI discovery adapter on top of PR #139; its 47/47 check-runs are on that dependent branch chain, not current main.
- Neither branch is an integrated main implementation. Their code and test assumptions must be reconciled against current main before use.

**Required closure:** range-aware API with explicit host bridge bounds; bridge subordinate-range enforcement; invalid/stale handle behavior; host tests for zero/nonzero starts, nested bridges, malformed ranges and cycles; x86-64/ARM64 builds and QEMU tests on exact final SHA.

## F-004 — Boot handoff contract is not unified in current main

**Severity:** Architecture gap  
**Confidence:** Medium — source/tree inventory; detailed boot-path review remains open  
**Master-plan mapping:** A, B, L, M, N; streams 1, 10, 11, 12.

The x86-64 entry path receives Multiboot2 `magic` and `addr` in `kernel_main()`; ARM64 platform bring-up receives the FDT directly in `arch/aarch64/main.c`. No common `BootInfo` contract/module is present in the current main tree. This does not prove that the current boot paths fail; it means the cross-firmware/platform handoff contract is not yet unified and versioned in main.

Sources:
- [kernel.c at baseline SHA, kernel_main entry](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/kernel.c#L87-L105)
- [ARM64 main entry](https://github.com/Masuku4129/kira_os_/blob/811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4/arch/aarch64/main.c)

**Required closure:** define versioned BootInfo/platform handoff, memory map ownership, modules/initrd, framebuffer, command line, firmware tables, DMA/IOMMU topology and lifetime; add BIOS/UEFI/ARM64 producer-consumer tests.

## F-005 — Device Model is not yet a bus-independent lifecycle in main

**Severity:** Architecture gap / integration blocker  
**Confidence:** Medium-high — current source layout and PR dependency evidence  
**Master-plan mapping:** A, G, K, M, N; streams 5, 6, 11, 12.

Main contains a PCI device array and KDRV binding table, but no bus-independent registry with stable identity/generation, resource ownership and a generic driver lifecycle contract. PR #141 introduces a registry/PCI adapter, but depends on PR #139 and PR #135 and is not in main.

**Required closure:** define DEV-001 identity/generation and resource ownership, enumerate devices through bus adapters, bind/unbind drivers through a common lifecycle, and prove stale-reference, failure-unwind, hotplug/rescan and resource-revocation behavior.

## Verification ledger

| Finding | Source reviewed | Runtime reproduction | Targeted regression in current main | Status |
|---|---|---|---|---|
| F-001 | x86-64 and ARM64 stack allocator | Not run in this session | Not present in current main host suite | CONFIRMED SOURCE DEFECT; runtime test pending |
| F-002 | KDRV, syscall capability gate, ARM64 DMA/MSI paths | Not run in this session | PR #136/137 tests are branch-only | CONFIRMED SOURCE SECURITY GAP |
| F-003 | PCI ECAM backend and scan root | Not run in this session | PR #139 tests are branch-only | CONFIRMED SOURCE DEFECT |
| F-004 | x86-64 and ARM64 entry paths/tree | Not run in this session | No unified BootInfo test in main | ARCHITECTURE GAP |
| F-005 | PCI/KDRV structure and PR #141 dependency | Not run in this session | No generic registry test in main | ARCHITECTURE GAP |

No code was changed by this audit. No test was executed locally. Exact-head CI evidence from other PRs is explicitly branch-only and is not transferred to main.
