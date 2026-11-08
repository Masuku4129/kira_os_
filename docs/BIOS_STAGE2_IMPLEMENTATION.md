# BIOS Stage-2 implementation contract

**Status:** implementation specification for `gpt/bios-stage2`; BIOS boot is not yet supported by this path.

## Why the current path cannot boot the current kernel

The current `arch/x86_64/bios/stage2.S`:
- accepts ELFCLASS32 (`e_ident[EI_CLASS] == 1`) and EM_386 (machine 3), while the production kernel is ELFCLASS64 / EM_X86_64;
- constructs a private 32-byte `KIB1`-like record at 0x7000, but the common x86-64 entry path expects Multiboot2 magic `0x36d76289` and a valid Multiboot2 information block in EBX;
- enters a 32-bit protected-mode call path that is not the kernel's supported entry ABI;
- has an invalid BSS zero-fill sequence: after restoring EDI to the program-header pointer, it replaces EDI with p_vaddr and then dereferences offsets from the destination address instead of reading p_memsz/p_filesz from the program header;
- is not referenced by the production ISO build script and has no BIOS disk-image builder or BIOS QEMU test.

These are source-level incompatibilities; no successful BIOS boot has been demonstrated.

## Required handoff contract

1. Stage 1 remains a 512-byte MBR-compatible sector with signature 0xAA55 and EDD-only disk reads. Its current fixed 32-sector transfer loads 16 KiB at physical 0x8000, so the current manifest address 0x9000 overlaps the loaded stage-2 extent unless the layout/loader count is redesigned. The builder must calculate the actual stage-2 sector count and place the manifest outside all loaded code/data; do not retain a fixed address that overlaps.
2. Stage 2 loads the actual x86-64 ELF kernel, not an ELF32 compatibility image. Validate:
   - ELF magic, ELFCLASS64, little-endian, ET_EXEC, EM_X86_64;
   - exact ELF header/program-header sizes and bounded program-header count;
   - all offset/size arithmetic without overflow;
   - every PT_LOAD has p_filesz <= p_memsz, file range within the image, and a physical destination range supported by the loader;
   - reject unsupported high 32-bit address/size fields rather than truncating them;
   - reject overlapping or reserved-memory destinations.
3. Copy PT_LOAD file bytes to the validated physical destination and zero exactly p_memsz - p_filesz bytes. The ELF entry must be within an executable loaded segment.
4. Collect E820 entries using INT 15h E820 with SMAP signature checks, bounded count, and carry/continuation handling. Do not fabricate usable memory.
5. Build a valid Multiboot2 information block containing at minimum a memory-map tag (type 6, entry_size 24) and end tag, with 8-byte tag alignment and a correct total_size.
6. Transfer control to the ELF entry in 32-bit protected mode with EAX=0x36D76289 and EBX=physical address of the Multiboot2 info block. The existing `arch/x86_64/boot.s` entry is specifically a dual-mode entry: its 32-bit path saves EAX/EBX, builds the initial identity map, enables PAE/paging/long mode, and only then calls `kernel_main` using the System V x86-64 ABI (RDI=magic, RSI=info pointer). Stage 2 must NOT build a second long-mode transition and must NOT call `kernel_main` directly. Preserve the kernel entry's expected 32-bit protected-mode state and low-memory constraints.
7. Stage 2, manifest, E820 buffer, stack, kernel staging buffer, and Multiboot2 info block must occupy explicitly reserved, non-overlapping physical ranges. The image builder must calculate and validate disk LBAs/sectors rather than relying on an undocumented fixed manifest address.

## Required build and runtime proof

- A deterministic host-side image builder must create a raw BIOS disk image from the actual stage1, stage2 and production `kira.bin`, with checks for stage sizes, manifest bounds, sector counts and ELF type.
- A test must verify the MBR signature and exact on-disk layout.
- CI must boot that image with QEMU's SeaBIOS (not OVMF, not GRUB) and capture a kernel UART marker emitted after `kernel_main` accepts the Multiboot2 handoff. A timeout, reset, triple fault, or marker absence is failure.
- The test must also exercise malformed ELF headers, out-of-range program headers, arithmetic overflow, invalid PT_LOAD ranges, missing E820 entries and a truncated disk image; each must fail closed without jumping to the kernel.
- The production ISO build must explicitly include this tested BIOS path. The release gate must not call BIOS supported until the SeaBIOS QEMU test passes.

## Scope and compatibility

The current UEFI and GRUB/Multiboot2 paths must remain unchanged. Do not add GRUB as a hidden dependency for the standalone BIOS disk path. Do not claim physical-hardware validation from QEMU results.


## Entry-contract correction (2026-11-08)

The first draft overstated the work required from stage 2. Inspection of `arch/x86_64/boot.s` confirms that the kernel ELF entry itself accepts 32-bit protected-mode Multiboot2 registers, saves EAX/EBX, constructs its initial 1 GiB identity map, enables PAE/paging/long mode, and calls `kernel_main` with RDI/RSI. Therefore stage 2 must hand off to the ELF entry in 32-bit protected mode; it must not perform the long-mode transition itself. The current stage-1 DAP also loads 32 sectors (16 KiB) at 0x8000 while stage 2 hardcodes its manifest at 0x9000, which lies inside that load range and is an additional layout defect to remove.


## Additional Stage-2 memory/DAP findings (2026-11-08)

Static inspection of the current `arch/x86_64/bios/stage2.S` found two further blockers that the implementation must address:

1. **Kernel DAP destination is truncated.** The source sets `KERNEL_BUFFER = 0x10000`, then executes `movw $KERNEL_BUFFER, %ax` followed by `movw %ax, dap_offset` and sets `dap_segment = 0`. A 16-bit offset cannot represent 0x10000; it becomes zero. The first BIOS read therefore targets physical 0x00000, not 0x10000, overwriting the IVT/BDA/low memory. For the intended 0x10000 physical destination, encode a valid segment:offset pair (for example 0x1000:0x0000), and test the computed linear address. Keep subsequent chunk advancement correct across 64-KiB boundaries.

2. **Kernel staging read has no bounded destination capacity.** `kernel_sectors` is accepted from the manifest and repeatedly read in chunks of up to 32 sectors. `remaining_bytes` is initialized but never used to enforce a limit. A large or malicious sector count can make the BIOS read overwrite arbitrary physical memory before ELF validation. Define a maximum staging extent based on an explicit reserved-memory map, reject zero/oversized counts and arithmetic overflow before the first disk read, and ensure the entire destination remains below the chosen ceiling and does not overlap Stage2, stack, E820 storage, BootInfo or the kernel's eventual PT_LOAD destinations.

Required regression checks: assert the DAP's initial segment:offset resolves exactly to 0x10000; test chunk transitions around 64-KiB boundaries; reject a manifest whose sector count exceeds the reserved staging capacity before issuing INT 13h; and verify no read destination overlaps reserved regions.
