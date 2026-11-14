# ADR-0005: PCI enumeration is bounded by host and bridge bus windows

- **Status:** Proposed; source defect confirmed, implementation not yet started.
- **Date:** 2026-11-14
- **Owners:** KIRA OS HAL / PCI Device Model

## Context

The PCI ECAM host adapter stores a declared `bus_start..bus_end` range. ARM64 FDT PCI host discovery passes its `bus-range` into `pci_use_ecam()`. However, the common `pci_init()` currently begins enumeration at bus 0. An ECAM host whose root bus is nonzero therefore returns all-ones config reads and discovers no devices.

The current bridge traversal reads only the secondary bus number and recursively scans it. It does not carry the host range or each ancestor bridge's subordinate bus limit through recursion. A malformed or misconfigured bridge can therefore cause scans outside its declared subordinate window (subject only to the ECAM adapter's outer range).

## Decision

1. Common PCI enumeration starts at the host adapter's declared root bus, not a hard-coded bus 0.
2. Every config-space read during recursive enumeration must remain inside the host adapter's bus range.
3. PCI-to-PCI bridge traversal validates primary, secondary and subordinate bus numbers:
   - primary must match the bus containing the bridge;
   - secondary must be greater than the primary;
   - secondary must not exceed subordinate;
   - subordinate must not exceed the host range or the parent bridge's subordinate limit.
4. Nested bridge scans inherit the parent's subordinate limit. Cycles and repeated bus visits remain guarded.
5. The legacy x86 port-based `pci_init()` behavior remains compatible: root bus 0, bridge recursion bounded by the configured bus-number fields.
6. A host-range or bridge-window violation must stop that traversal safely; it must not be silently widened to bus 0..255.

## Verification requirements

- Host model with a nonzero root bus discovers the root function.
- A bridge discovers a child endpoint only inside its secondary/subordinate window.
- Nested bridges cannot escape the parent subordinate window.
- A secondary bus outside the host range is not read.
- Invalid primary/secondary/subordinate tuples are rejected.
- Legacy `pci_init()` still enumerates bus 0 and valid downstream bridges.
- x86-64 and ARM64 builds plus QEMU scenarios pass; ARM64 FDT `bus-range` handoff is verified.
- Exact-head SHA and complete CI matrix are recorded.

## Source evidence

- `pci_use_ecam()` stores `bus_start` and `bus_end`.
- `pci_init()` currently calls `scan_bus(0)`.
- `scan_fn()` currently reads bridge secondary bus `0x19` and recurses without carrying subordinate limits.
- ARM64 `arm_pci_init()` passes FDT `bus-range` to `pci_use_ecam()` before `pci_init()`.

This ADR records the required contract; it does not claim the current implementation satisfies it.
