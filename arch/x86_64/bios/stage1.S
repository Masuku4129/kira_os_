/*
 * KIRA BIOS Stage 1 — 16-bit MBR-compatible bootstrap.
 *
 * Disk layout contract:
 *   LBA 0: this 512-byte sector
 *   LBA 1..N: contiguous BIOS stage-2 image
 *
 * The installer must patch STAGE2_LBA and STAGE2_SECTORS and place the
 * stage-2 image in the reserved contiguous extent. This sector uses INT 13h
 * extensions and refuses disks without EDD support.
 */

.code16
.section .text
.global _start

.set STAGE2_LOAD_SEG, 0x0000
.set STAGE2_LOAD_OFF, 0x8000
.set STAGE2_LBA,      1
.set STAGE2_SECTORS,  32
.set BOOT_DRIVE_ADDR, 0x0500

_start:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7c00, %sp
    sti
    cld

    movb %dl, boot_drive
    movb %dl, BOOT_DRIVE_ADDR

    /* Require INT 13h Extensions (EDD). */
    movw $0x55aa, %bx
    movb boot_drive, %dl
    movb $0x41, %ah
    int $0x13
    jc disk_error
    cmpw $0xaa55, %bx
    jne disk_error
    testw $1, %cx
    jz disk_error

    /* Read stage 2 with a 16-byte Disk Address Packet. */
    movw $dap, %si
    movb boot_drive, %dl
    movb $0x42, %ah
    int $0x13
    jc disk_error

    /* Transfer control to the loaded stage 2 at 0000:8000. */
    cli
    ljmp $STAGE2_LOAD_SEG, $STAGE2_LOAD_OFF

disk_error:
    movw $error_text, %si
    call print_string
halt:
    cli
    hlt
    jmp halt

print_string:
    lodsb
    testb %al, %al
    jz 1f
    movb $0x0e, %ah
    movw $0x0007, %bx
    int $0x10
    jmp print_string
1:
    ret

boot_drive:
    .byte 0

.align 4
dap:
    .byte 0x10
    .byte 0
    .word STAGE2_SECTORS
    .word STAGE2_LOAD_OFF
    .word STAGE2_LOAD_SEG
    .quad STAGE2_LBA

error_text:
    .asciz "KIRA: BIOS EDD boot failed"

.org 510
.word 0xaa55
