/*
 * KIRA BIOS Stage 2 — protected-mode transition and ELF32 kernel loader.
 *
 * Stage 2 is loaded at 0000:8000 by stage 1. The installer places a compact
 * manifest immediately after this binary; manifest fields are patched by
 * the image builder:
 *   magic "K2MF", kernel LBA (u64), kernel sectors (u32), load address (u32)
 *
 * This loader reads the kernel image into 0x10000, validates ELF32 headers,
 * copies PT_LOAD segments, creates a flat GDT and enters 32-bit protected mode.
 * Kernel entry is passed EAX=KIRA_BOOT_MAGIC, EBX=physical BootInfo address.
 */

.code16
.section .text
.global stage2_entry

.set BOOT_MAGIC, 0x4b495241
.set KERNEL_BUFFER, 0x10000
.set BOOTINFO_ADDR, 0x7000
.set MANIFEST_ADDR, 0x9000
.set MAX_PHDRS, 32
.set ELF32_EHDR_SIZE, 52
.set ELF32_PHDR_SIZE, 32

stage2_entry:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw $0x7c00, %sp
    sti
    cld
    movb %dl, boot_drive

    /* Manifest is stored at the fixed address immediately following stage 2. */
    movw $MANIFEST_ADDR, %si
    cmpl $0x464d324b, (%si)
    jne stage2_error
    movl 12(%si), %eax
    testl %eax, %eax
    jz stage2_error
    cmpl $0x10000, 16(%si)
    jne stage2_error

    movl 4(%si), %eax
    movl %eax, kernel_lba
    movl 8(%si), %eax
    movl %eax, kernel_lba+4
    movl 12(%si), %eax
    movl %eax, kernel_sectors

    /* Read kernel in EDD chunks, at most 32 sectors per request. */
    movl kernel_lba, %eax
    movl %eax, dap_lba
    movl kernel_lba+4, %eax
    movl %eax, dap_lba+4
    movl $KERNEL_BUFFER, %eax
    movl %eax, remaining_bytes
    movw $KERNEL_BUFFER, %ax
    movw %ax, dap_offset
    movw $0, dap_segment

read_kernel:
    movl kernel_sectors, %eax
    testl %eax, %eax
    jz kernel_loaded
    cmpl $32, %eax
    jbe 1f
    movl $32, %eax
1:
    movw %ax, dap_count
    movw $dap, %si
    movb boot_drive, %dl
    movb $0x42, %ah
    int $0x13
    jc stage2_error

    movzwl dap_count, %eax
    addl %eax, dap_lba
    adcl $0, dap_lba+4
    subl %eax, kernel_sectors

    shll $9, %eax
    addw %ax, dap_offset
    jnc read_kernel
    addw $0x1000, dap_segment
    jmp read_kernel

kernel_loaded:
    /* Validate ELF magic and 32-bit little-endian executable class. */
    cmpl $0x464c457f, KERNEL_BUFFER
    jne stage2_error
    cmpb $1, KERNEL_BUFFER+4
    jne stage2_error
    cmpb $1, KERNEL_BUFFER+5
    jne stage2_error
    cmpw $2, KERNEL_BUFFER+16
    jne stage2_error
    cmpw $3, KERNEL_BUFFER+18
    jne stage2_error
    cmpw $ELF32_EHDR_SIZE, KERNEL_BUFFER+40
    jne stage2_error
    cmpw $ELF32_PHDR_SIZE, KERNEL_BUFFER+42
    jne stage2_error

    movl KERNEL_BUFFER+28, %eax
    movl %eax, phdr_offset
    movzwl KERNEL_BUFFER+44, %eax
    cmpl $MAX_PHDRS, %eax
    ja stage2_error
    movl %eax, phdr_count
    testl %eax, %eax
    jz stage2_error

    /* Copy each PT_LOAD segment from the in-memory ELF image. */
    movl $0, phdr_index
copy_phdr:
    movl phdr_index, %eax
    cmpl phdr_count, %eax
    jae segments_loaded

    movl phdr_offset, %edi
    addl $KERNEL_BUFFER, %edi
    movl phdr_index, %eax
    imull $ELF32_PHDR_SIZE, %eax
    addl %eax, %edi

    cmpl $1, (%edi)
    jne next_phdr

    movl 4(%edi), %esi
    addl $KERNEL_BUFFER, %esi
    movl 8(%edi), %ebx
    movl 16(%edi), %ecx
    movl 20(%edi), %edx

    cmpl %edx, %ecx
    ja stage2_error
    testl %edx, %edx
    jz next_phdr

    /* Reject destinations below 1 MiB or above the 32-bit physical range. */
    cmpl $0x00100000, %ebx
    jb stage2_error
    movl %ebx, %eax
    addl %edx, %eax
    jc stage2_error

    pushl %esi
    pushl %edi
    pushl %ecx
    pushl %edx
    movl %ebx, %edi
    rep movsb
    popl %edx
    popl %ecx
    popl %edi
    popl %esi

    /* Zero-fill p_memsz - p_filesz. */
    movl 8(%edi), %edi
    addl 20(%edi), %edi
    movl 20(%edi), %eax
    subl 16(%edi), %eax
    movl %eax, %ecx
    xorl %eax, %eax
    rep stosb

next_phdr:
    incl phdr_index
    jmp copy_phdr

segments_loaded:
    movl KERNEL_BUFFER+24, %eax
    movl %eax, kernel_entry

    /* Minimal BootInfo v1 block: magic, version, size, boot drive, flags. */
    movl $0x3142494b, BOOTINFO_ADDR
    movl $1, BOOTINFO_ADDR+4
    movl $32, BOOTINFO_ADDR+8
    movzbl boot_drive, %eax
    movl %eax, BOOTINFO_ADDR+12
    movl $0, BOOTINFO_ADDR+16
    movl $0, BOOTINFO_ADDR+20
    movl $0, BOOTINFO_ADDR+24
    movl $0, BOOTINFO_ADDR+28

    cli
    lgdt gdt_descriptor
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmp $0x08, $protected_entry

stage2_error:
    movw $error_text, %si
    call print_string
fatal:
    cli
    hlt
    jmp fatal

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

.align 4
gdt:
    .quad 0
    .quad 0x00cf9a000000ffff
    .quad 0x00cf92000000ffff
gdt_end:
gdt_descriptor:
    .word gdt_end - gdt - 1
    .long gdt

boot_drive:      .byte 0
kernel_lba:      .quad 0
kernel_sectors:  .long 0
remaining_bytes: .long 0
phdr_offset:     .long 0
phdr_count:      .long 0
phdr_index:      .long 0
kernel_entry:    .long 0

.align 4
dap:
    .byte 0x10, 0
dap_count:   .word 0
dap_offset:  .word KERNEL_BUFFER
dap_segment: .word 0
dap_lba:     .quad 0

error_text:
    .asciz "KIRA: invalid BIOS kernel image"

.code32
protected_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movl $0x9f000, %esp
    pushl $BOOTINFO_ADDR
    pushl $BOOT_MAGIC
    movl kernel_entry, %eax
    call *%eax
halt32:
    cli
    hlt
    jmp halt32
