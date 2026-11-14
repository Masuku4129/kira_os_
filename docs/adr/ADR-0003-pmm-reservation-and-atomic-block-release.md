# ADR-0003: PMM reservation ownership and atomic contiguous release

- **Status:** Proposed; x86-64 implementation verified in Draft PR #154; ARM64 parity implementation under exact-head CI in Draft PR #158.
- **Date:** 2026-11-14
- **Owners:** KIRA OS kernel architecture

## Context

The physical memory manager has three materially different page states:

1. **Free:** eligible for allocation.
2. **Allocated:** owned by a live kernel subsystem, process, driver or page-table structure.
3. **Reserved:** unavailable to the allocator because it belongs to firmware handoff data, the kernel image, early allocations, page tables, DMA pools, MMIO backing reservations or another platform reservation.

A single used bit cannot distinguish allocated pages from reserved pages. If `pmm_free_page()` treats every used page as releasable, a stale or invalid free can return kernel/FDT/DMA memory to the allocator.

Contiguous allocations are also ownership units. Releasing them one page at a time permits partial release when a middle page is invalid or already free.

## Decision

- PMM must track reservation ownership separately from allocation state on every architecture.
- `pmm_free_page()` rejects unaligned, out-of-range, already-free and reserved pages.
- `pmm_free_contiguous()` validates the entire aligned range and ownership under one PMM lock before changing any page state.
- If any page fails validation, no page in the range is released and free-page counters remain unchanged.
- Overlapping firmware RAM regions are counted once; pages outside validated RAM remain unavailable.
- x86-64 and ARM64 must implement the same externally observable contract, even if their internal bitmap layouts differ.

## Consequences

- The ARM64 PMM requires separate used and reserved bitmaps; bitmap sizing must include both.
- Reservations may be added after initialization and remain non-freeable.
- All DMA, page-table, shared-memory and device teardown paths can rely on all-or-nothing contiguous release.
- PMM map-size arithmetic and region bounds must be checked before bitmap indexing.

## Verification requirements

- Host tests on both implementations cover reserved-page rejection, double free, out-of-range/unaligned addresses, overlapping RAM regions and invalid-middle contiguous release with unchanged free count.
- ASan/UBSan host suite passes.
- x86-64 and ARM64 builds pass.
- QEMU memory, DMA, storage, SMP and driver scenarios pass on the exact tested SHA.
- Physical hardware support remains unverified until named hardware is tested.
