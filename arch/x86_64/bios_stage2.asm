bits 16
org 0x8000
%include "bios_layout.inc"

%define MB2_MAGIC 0x36D76289
%define E820_BUFFER 0x6000
%define E820_MAX 128
%define MBI_BUFFER 0x5000
%define BOUNCE_SEG 0x1000
%define READ_SECTORS 64

stage2_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [boot_drive], dl

    call collect_e820
    jc stage2_error
    call enable_a20
    call clear_kernel_memory
    call load_kernel
    jc stage2_error
    call build_multiboot_info

    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:protected_entry

; ---------------------------------------------------------------------------
; BIOS E820 -> a bounded 24-byte entry array at physical 0x6000.
; ---------------------------------------------------------------------------
collect_e820:
    xor ebx, ebx
    mov word [e820_count], 0
.next:
    mov ax, [e820_count]
    cmp ax, E820_MAX
    jae .failed
    mov bx, 24
    mul bx
    add ax, E820_BUFFER
    mov di, ax
    xor ax, ax
    mov es, ax
    mov cx, 24
    xor al, al
    rep stosb
    ; DI was advanced by STOSB: recompute the descriptor destination.
    mov ax, [e820_count]
    mov bx, 24
    mul bx
    add ax, E820_BUFFER
    mov di, ax
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    int 0x15
    jc .bios_end
    cmp eax, 0x534D4150
    jne .failed
    cmp ecx, 20
    jb .failed
    cmp ecx, 24
    jae .check_attr
    mov dword [es:di+20], 1
    jmp .accept
.check_attr:
    test dword [es:di+20], 1
    jz .continue
.accept:
    inc word [e820_count]
.continue:
    test ebx, ebx
    jnz .next
    cmp word [e820_count], 0
    je .failed
    clc
    ret
.bios_end:
    cmp word [e820_count], 0
    je .failed
    clc
    ret
.failed:
    stc
    ret

; ---------------------------------------------------------------------------
; Enable A20 through the fast gate.
; ---------------------------------------------------------------------------
enable_a20:
    in al, 0x92
    or al, 2
    and al, 0xFE
    out 0x92, al
    ret

; ---------------------------------------------------------------------------
; Zero the full linked kernel memory span first; objcopy omits NOBITS .bss.
; ---------------------------------------------------------------------------
clear_kernel_memory:
    call enter_unreal
    mov edi, 0x00100000
    mov ecx, KERNEL_MEM_BYTES
    xor eax, eax
    a32 rep stosb
    ret

; ---------------------------------------------------------------------------
; Load the flat image at 1 MiB using EDD reads to a low bounce buffer,
; then copy each chunk above 1 MiB in unreal mode.
; ---------------------------------------------------------------------------
load_kernel:
    mov dword [disk_lba], KERNEL_LBA
    mov dword [destination], 0x00100000
    mov dword [remaining], KERNEL_BYTES
.loop:
    cmp dword [remaining], 0
    je .done
    mov eax, [remaining]
    add eax, 511
    shr eax, 9
    cmp eax, READ_SECTORS
    jbe .count_ready
    mov eax, READ_SECTORS
.count_ready:
    mov [dap+2], ax
    mov word [dap+4], 0
    mov word [dap+6], BOUNCE_SEG
    mov eax, [disk_lba]
    mov [dap+8], eax
    mov dword [dap+12], 0

    mov si, dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc .failed
    xor ax, ax
    mov ds, ax

    movzx eax, word [dap+2]
    shl eax, 9
    cmp eax, [remaining]
    jbe .copy_count_ready
    mov eax, [remaining]
.copy_count_ready:
    mov [copy_bytes], eax

    call enter_unreal
    mov ax, BOUNCE_SEG
    mov ds, ax
    xor esi, esi
    mov edi, [cs:destination]
    mov ecx, [cs:copy_bytes]
    a32 rep movsb
    xor ax, ax
    mov ds, ax

    mov eax, [copy_bytes]
    add [destination], eax
    sub [remaining], eax
    movzx eax, word [dap+2]
    add [disk_lba], eax
    jmp .loop
.done:
    clc
    ret
.failed:
    stc
    ret

; Enter protected mode briefly to load a 4 GiB ES descriptor, then return to
; real mode while preserving the cached segment limit (unreal mode).
enter_unreal:
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:unreal_pm_entry
bits 32
unreal_pm_entry:
    mov ax, 0x10
    mov es, ax
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax
    jmp 0x0000:unreal_rm_entry
bits 16
unreal_rm_entry:
    sti
    ret

; ---------------------------------------------------------------------------
; Synthesize a minimal valid Multiboot2 information block with the E820 map.
; Kernel scans BIOS memory for RSDP and keeps VGA text mode if no GOP exists.
; ---------------------------------------------------------------------------
build_multiboot_info:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov dword [MBI_BUFFER], 0
    mov dword [MBI_BUFFER+4], 0
    mov dword [MBI_BUFFER+8], 6
    movzx eax, word [e820_count]
    imul eax, 24
    add eax, 16
    mov [MBI_BUFFER+12], eax
    mov dword [MBI_BUFFER+16], 24
    mov dword [MBI_BUFFER+20], 0

    mov si, E820_BUFFER
    mov di, MBI_BUFFER+24
    mov cx, [e820_count]
    imul cx, 24
    rep movsb

    ; Unknown E820 types are treated as reserved by Multiboot2.
    mov cx, [e820_count]
    mov di, MBI_BUFFER+24+16
.normalize:
    test cx, cx
    jz .normalized
    cmp dword [di], 1
    jb .reserved
    cmp dword [di], 5
    jbe .next
.reserved:
    mov dword [di], 2
.next:
    add di, 24
    dec cx
    jmp .normalize
.normalized:
    mov di, MBI_BUFFER+24
    mov cx, [e820_count]
    imul cx, 24
    add di, cx
    mov dword [di], 0
    mov dword [di+4], 8
    add di, 8
    mov ax, di
    sub ax, MBI_BUFFER
    movzx eax, ax
    mov [MBI_BUFFER], eax
    ret

; ---------------------------------------------------------------------------
; Failure path: visible BIOS text, then halt.
; ---------------------------------------------------------------------------
stage2_error:
    xor ax, ax
    mov ds, ax
    mov si, error_text
.print:
    lodsb
    test al, al
    jz .halt
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp .print
.halt:
    cli
    hlt
    jmp .halt

boot_drive db 0
e820_count dw 0
disk_lba dd 0
destination dd 0
remaining dd 0
copy_bytes dd 0
error_text db "KIRA BIOS: E820 or kernel load failed",0

align 4
dap:
    db 0x10,0
    dw READ_SECTORS
    dw 0
    dw BOUNCE_SEG
    dq 0

align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt_end:
gdt_descriptor:
    dw gdt_end-gdt-1
    dd gdt

bits 32
protected_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, MB2_MAGIC
    mov ebx, MBI_BUFFER
    jmp KERNEL_ENTRY

times STAGE2_SECTORS*512-($-$$) db 0
