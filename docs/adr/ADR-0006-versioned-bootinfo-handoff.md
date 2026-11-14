# ADR-0006: Versioned cross-architecture BootInfo handoff

- Status: Proposed
- Date: 2026-11-14
- Scope: x86-64 and ARM64 boot entry, early memory ownership, firmware/resource discovery
- Related findings: F-004, F-005, F-013, F-015
- Related contracts: BootInfo, PMM reservation, Device Model, framebuffer, initrd/module lifetime

## Context

Current main has two different boot handoffs:
- x86-64 enters `kernel_main(magic, addr)` with Multiboot2 magic and a physical Multiboot2 information pointer. `mbinfo_relocate()` may copy the information block above the initial 1 GiB identity map.
- ARM64 enters `arm_main(dtb, entry_el)` with an FDT pointer in x0, then parses memory, reserved-memory, framebuffer, PSCI, GIC and PCI host data directly from the tree.

Both paths are functional platform adapters, but the kernel has no common, versioned object describing the resources and ownership transferred by the boot environment. As a result, early PMM and later subsystems consume architecture-specific data and implicit lifetime assumptions.

## Decision

Introduce an immutable, versioned `BootInfo` contract produced by architecture-specific boot adapters and consumed by common early kernel initialization.

The contract is a normalized handoff, not a replacement for ACPI or FDT. Raw firmware tables remain available as explicitly typed, bounded physical blobs when a subsystem needs platform-specific details.

### Required header

The v1 header must contain:
- fixed magic and ABI version;
- header size and total structure size;
- target architecture identifier;
- flags, with unknown mandatory flags rejected;
- physical address of the original handoff source, when present;
- bootloader/firmware identifier and bounded version string;
- physical address and count/entry size for the normalized memory map.

All sizes and offsets are validated with subtraction-based bounds checks. All physical ranges use half-open `[base, end)` semantics and reject zero length or arithmetic overflow where applicable.

### Normalized resource descriptors

The v1 memory map must preserve, at minimum:
- usable RAM;
- firmware/ACPI reclaimable and NVS memory;
- reserved or unusable memory;
- MMIO/device ranges;
- persistent memory, when reported;
- memory ownership/reservation reason where the producer can identify it.

Each entry contains physical base, length, normalized type, attributes and a stable owner/reason code. Unknown memory types are treated as reserved, never as free RAM.

### Optional handoff records

BootInfo may reference bounded records for:
- FDT blob (ARM64 and platforms that provide it);
- ACPI RSDP (x86-64 and ACPI-capable platforms);
- command line;
- initrd/modules (base, size, type and lifetime);
- framebuffer (base, size, width, height, pitch, pixel format);
- entropy seed (length-delimited; absence is explicit);
- DMA-reserved pools and IOMMU topology/resource descriptors when supplied by firmware.

An absent record is represented explicitly; zero must not ambiguously mean both “absent” and a valid address.

### Ownership and lifetime

- The boot adapter validates the source protocol and copies BootInfo plus normalized descriptors into kernel-owned memory before the source buffer can be reclaimed.
- Raw FDT/ACPI/module/framebuffer ranges remain reserved until the owning subsystem explicitly releases or permanently maps them.
- The PMM must reserve the kernel image, BootInfo copy, source handoff blob while still referenced, initrd/modules, framebuffer, firmware-reserved ranges, early page tables/allocations and DMA pools before exposing free pages.
- BootInfo is immutable after validation. Later discovery updates live in the Device Model/resource registry, not by mutating the handoff.
- Physical addresses are never silently converted to virtual pointers inside the ABI.

## Architecture adapters

1. x86-64 Multiboot2 adapter: validate magic and total size; translate memory-map tags, module tags, command line, framebuffer and ACPI pointers into BootInfo. Keep the original MBI reserved until the adapter has copied all required fields and any retained raw blobs have explicit ownership.
2. ARM64 FDT adapter: validate FDT bounds; normalize `/memory`, `/reserved-memory`, framebuffer and boot arguments; retain FDT physical range; extract PSCI/GIC/PCI host data as typed platform records or leave the raw FDT available to the platform drivers.
3. Future UEFI adapter: translate EFI memory descriptors and configuration-table pointers before ExitBootServices; no UEFI service calls are permitted after handoff.

## Initialization order

`arch entry -> source-protocol validation -> BootInfo construction/copy -> reservation ledger -> PMM initialization -> firmware/platform table parsing -> Device Model discovery -> drivers/services`.

No allocator may treat memory as free before the reservation ledger is committed. The adapter must fail closed on malformed mandatory memory data; it must not guess a memory map from a default address or silently discard invalid reserved ranges.

## Compatibility and migration

- Keep current x86-64 and ARM64 entry paths operational while introducing adapters.
- Migrate consumers one subsystem at a time; do not remove Multiboot2/FDT parsers until the common path and all dependent tests are in place.
- ABI changes require a version increment and explicit producer/consumer compatibility tests.
- Do not synthesize fake modules, framebuffer, entropy, IOMMU or ACPI records when firmware did not provide them.

## Acceptance criteria

- Host tests: valid/invalid headers, truncated records, integer overflow, unknown optional/mandatory flags, malformed memory entries, overlapping ranges and absent optional records.
- x86-64 producer tests: Multiboot2 memory map, command line, modules, framebuffer, ACPI pointer and high-address MBI copy/reservation.
- ARM64 producer tests: FDT memory/reserved-memory, framebuffer, command line, PSCI/GIC/PCI references and malformed FDT rejection.
- PMM integration tests prove every reserved range remains unavailable and every usable range is represented exactly once.
- QEMU x86-64 and ARM64 boot matrix proves BootInfo reaches common initialization with no loss of current functionality.
- No source protocol or architecture adapter is marked complete based only on host models; exact-head build and QEMU evidence are required.

## Consequences

- Common kernel initialization no longer depends on Multiboot2 or FDT parser details.
- Memory ownership becomes explicit and testable before PMM exposes pages.
- Firmware-specific tables remain available without coupling common code to their formats.
- Migration requires careful ordering and temporary duplication of parsing; this is intentional to avoid breaking current boot paths.
