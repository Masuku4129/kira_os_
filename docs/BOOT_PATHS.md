# Native BIOS and UEFI boot paths

The regular `kira.iso` GRUB path remains available for compatibility.

## Native BIOS disk image

The build also produces `kira-bios.img`, a raw hard-disk image:

1. LBA 0 is a 16-bit MBR stage one. It checks INT 13h EDD support and reads the fixed-size stage two from LBA 1.
2. Stage two gathers the BIOS E820 memory map, enables A20, reads the flat kernel image through EDD into a low bounce buffer, and copies it above 1 MiB using unreal mode.
3. It clears the linked kernel memory span first so the NOBITS `.bss` region is zeroed, builds a bounded Multiboot2 memory-map handoff, enters 32-bit protected mode, and jumps to the linked `start` symbol.

The build also wraps this raw image as `kira-bios.iso` using El Torito hard-disk emulation. In Limbo, use `kira-bios.iso` as the CD-ROM and keep `kira_disk.img` as Hard Disk A; this preserves the normal primary-master KiraFS data disk. The raw `kira-bios.img` can also be attached directly as a boot hard disk for loader-only experiments.

## Native UEFI removable media

The build optionally creates `uefi-fat.img` and the `uefi/` directory when LLVM `lld-link`, `mkfs.vfat`, and mtools are installed. The FAT32 volume contains `EFI/BOOT/BOOTX64.EFI` and `KIRA.BIN`. The EFI application loads each ELF `PT_LOAD` segment into separate EFI loader-code or loader-data pages, verifies executable permissions through `EFI_MEMORY_ATTRIBUTE_PROTOCOL`, builds and validates the versioned KIRA BootInfo blob across the resulting UEFI memory descriptors, ACPI RSDP, and GOP data, exits boot services (with a map-only retry), and enters the native long-mode shim. UEFI firmware without the memory-attribute protocol is rejected rather than assuming newly allocated loader-code pages are executable.

The native UEFI files are a removable-media path; they are not yet the El Torito EFI boot image embedded in `kira.iso`.
