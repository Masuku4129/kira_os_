# Native UEFI boot files

After a successful `./build_clang.sh --no-run` on a host with LLVM `lld-link`,
the build creates:

- `uefi/EFI/BOOT/BOOTX64.EFI` — the native x86-64 UEFI application
- `uefi/KIRA.BIN` — the identity-linked ELF kernel with `uefi_entry` as ELF entry
- `uefi/EFI/BOOT/KIRA.BIN` — fallback copy for firmware volumes that expose the EFI directory as the root

For removable-media testing, format a USB drive as FAT32 and copy the complete
contents of `uefi/` to its root. Firmware starts `EFI/BOOT/BOOTX64.EFI`.
The loader reads `KIRA.BIN`, validates and loads its ELF64 PT_LOAD segments,
builds a Multiboot2 memory-map/ACPI/GOP handoff from UEFI tables, calls
`ExitBootServices`, installs KIRA's 1 GiB identity page tables, and enters
`uefi_entry`.

The existing GRUB ISO path remains unchanged. This native loader is a separate
removable-media boot path; it is not yet embedded as the ISO's El Torito EFI
boot image.
