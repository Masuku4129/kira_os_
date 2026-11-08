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

1. Stage 1 remains a 512-byte MBR-compatible sector with signature 0xAA55 and EDD-only disk reads.
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
6. Enter 32-bit protected mode with paging/PAE/long-mode transition requirements satisfied by the common kernel entry. Pass EAX=0x36D76289 and EBX=physical address of the Multiboot2 info block. The existing dual-mode prologue in `arch/x86_64/boot.s` must be reused; do not call the 64-bit kernel entry as a 32-bit C function.
7. Stage 2, manifest, E820 buffer, stack, kernel staging buffer, and Multiboot2 info block must occupy explicitly reserved, non-overlapping physical ranges. The image builder must calculate and validate disk LBAs/sectors rather than relying on an undocumented fixed manifest address.

## Required build and runtime proof

- A deterministic host-side image builder must create a raw BIOS disk image from the actual stage1, stage2 and production `kira.bin`, with checks for stage sizes, manifest bounds, sector counts and ELF type.
- A test must verify the MBR signature and exact on-disk layout.
- CI must boot that image with QEMU's SeaBIOS (not OVMF, not GRUB) and capture a kernel UART marker emitted after `kernel_main` accepts the Multiboot2 handoff. A timeout, reset, triple fault, or marker absence is failure.
- The test must also exercise malformed ELF headers, out-of-range program headers, arithmetic overflow, invalid PT_LOAD ranges, missing E820 entries and a truncated disk image; each must fail closed without jumping to the kernel.
- The production ISO build must explicitly include this tested BIOS path. The release gate must not call BIOS supported until the SeaBIOS QEMU test passes.

## Scope and compatibility

The current UEFI and GRUB/Multiboot2 paths must remain unchanged. Do not add GRUB as a hidden dependency for the standalone BIOS disk path. Do not claim physical-hardware validation from QEMU results.
