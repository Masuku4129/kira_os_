# Intel VT-d DMA isolation

The x86-64 boot path initializes PCI enumeration and VT-d before starting any
device driver. The backend consumes the checked DMAR inventory produced by
`acpi.c`.

## Unit initialization

- Bus mastering is cleared on every enumerated PCI function before the remapping
  units are touched.
- Firmware translation, queued-invalidation and interrupt-remapping modes are
  disabled with bounded status polling before KIRA replaces the root table.
- A unit is accepted only when it exposes 48-bit second-level translation,
  a 48-bit MGAW, coherent page walks (ECAP.C), a usable IOTLB register
  window and a supported register path. Leaf snoop bits are set only when
  ECAP.SC advertises support.
- The root/context tables and four-level second-level page tables are allocated
  from physical memory and mapped uncached for CPU access.
- Trusted in-kernel devices receive identity mappings for managed RAM and
  their device-scoped RMRR ranges. PCI bridges do not receive DMA domains.
- Untrusted devices receive no context entry until a signed/locally approved
  Ring 3 package requests DMA or MSI. Each such device gets a private,
  initially empty second-level root plus only its own RMRR mappings.

## DMA lifecycle

`KDRV_SYS_DMA_ALLOC` is accepted only for a device with a live private domain.
The kernel allocates contiguous pages, installs identity IOVA mappings, flushes
the IOTLB, and only then maps the pages into the driver process. If the IOTLB
cannot be invalidated, the physical pages remain pinned and are retried during
driver teardown. Teardown disables bus mastering before revoking translations
and returning pages to the physical allocator.

The DMA interface currently uses identity IOVAs and grants read/write access
to the requested allocation. It does not yet implement per-buffer DMA
direction, queued invalidation, interrupt remapping, ATS/PASID, scalable mode,
or non-identity IOVA allocation. Those features are not advertised as enabled.
Because interrupt remapping is not enabled, KDRV rejects `CAP_IRQ` packages;
second-level DMA isolation alone is not treated as MSI isolation.

## Platform boundaries

Only segment 0 is supported. Explicit DMAR scopes are resolved through PCI
bridge paths; an include-all unit is used only when routing is unambiguous.
Malformed or ambiguous routes fail closed. If no usable VT-d unit exists,
Ring 3 DMA/MSI stays denied.

ARM64 remains separately guarded: its SMMUv3 command-queue work is not yet a
complete device-domain backend, so Ring 3 DMA/MSI remains denied there.

## Verification status

This document describes implemented source paths, not successful hardware
validation. No VT-d hardware test, QEMU DMAR test, x86 build, or full CI run is
claimed by this change set yet. Those checks are intentionally deferred until
the implementation pass is complete.
