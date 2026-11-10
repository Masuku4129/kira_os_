; KIRA BIOS stage 2 (загружается stage1 в 0000:8000, размер ровно 16 КиБ).
; Делает: A20 -> карта памяти E820 (в виде Multiboot2 info) -> чтение ядра ELF64 по LBA из манифеста в
; промежуточную область 16 МиБ (через буфер 0x10000 и unreal mode) -> protected mode -> CRC32 файла ->
; строгая проверка ELF64 и E820 -> копирование PT_LOAD по p_paddr, обнуление bss -> вход в ядро
; (EAX=0x36D76289, EBX=адрес MB2 info) - тот же контракт, что у GRUB, поэтому boot.s не меняется.
; Манифест (последние 32 байта образа stage 2) заполняет tools/mkbios.py.
bits 16
org 0x8000

INFO        equ 0x1000          ; MB2 info: 0x1000..0x1FFF
BOUNCE_SEG  equ 0x1000          ; буфер чтения 0x10000..0x13FFF (32 сектора)
STAGE_BASE  equ 0x1000000       ; промежуточное место файла ядра
MAXE        equ 96              ; число записей E820, помещающихся в INFO
CHUNK       equ 32
S2_BYTES    equ 16384
MAX_KERNEL  equ 0x3000000       ; 48 МиБ
VBE_CTRL    equ 0x3000          ; ответ 4F00 (512 байт)

%macro SPUT 0                   ; AL -> COM1 (с ожиданием)
  push edx
  push eax
  mov dx, 0x3FD
%%w: in al, dx
  test al, 0x20
  jz %%w
  pop eax
  mov dx, 0x3F8
  out dx, al
  pop edx
%endmacro

%macro CONTAINED 0              ; EAX = начало, EBX = конец (искл.): CF=0, если целиком в одной записи type 1
  push esi
  push ecx
  push edx
  mov ecx, [e_count]
  mov esi, INFO + 24
%%l:
  test ecx, ecx
  jz %%no
  cmp dword [esi + 16], 1
  jne %%n
  cmp dword [esi + 4], 0
  jne %%n
  mov edx, [esi]
  cmp edx, eax
  ja %%n
  cmp dword [esi + 12], 0
  jne %%yes
  add edx, [esi + 8]
  jc %%yes
  cmp edx, ebx
  jae %%yes
%%n:
  add esi, 24
  dec ecx
  jmp %%l
%%yes:
  clc
  jmp %%o
%%no:
  stc
%%o:
  pop edx
  pop ecx
  pop esi
%endmacro

; ---------------------------------------------------------------- 16 бит
start16:
  cli
  xor ax, ax
  mov ds, ax
  mov es, ax
  mov ss, ax
  mov sp, 0x7C00
  cld
  mov [drive], dl
  call serial_init
  call cd_detect
  jc near fail_disk
  mov si, m_banner
  call print16
  cmp dword [man_magic], 'KBM1'
  jne near fail_manifest
  mov eax, [man_size]
  cmp eax, 64
  jb near fail_manifest
  cmp eax, MAX_KERNEL
  ja near fail_manifest
  cmp dword [man_flags], 0
  je .no_module_manifest
  cmp dword [man_flags], 1
  jne near fail_manifest
  cmp dword [man_mod_size], 0
  je near fail_manifest
  cmp dword [man_mod_size], 0x04000000
  ja near fail_manifest
  test dword [man_mod_lba], 3
  jnz near fail_manifest
  mov eax, [man_size]
  add eax, 511
  jc near fail_manifest
  shr eax, 9
  add eax, [man_lba]
  jc near fail_manifest
  cmp [man_mod_lba], eax
  jb near fail_manifest
  jmp .module_manifest_ok
.no_module_manifest:
  cmp dword [man_mod_lba], 0
  jne near fail_manifest
  cmp dword [man_mod_size], 0
  jne near fail_manifest
  cmp dword [man_mod_crc], 0
  jne near fail_manifest
.module_manifest_ok:
  mov dword [mod_stage_base], 0
  cmp dword [man_mod_size], 0
  je .no_module_stage
  mov eax, [man_size]
  add eax, 4095
  jc near fail_stage
  and eax, ~4095
  add eax, STAGE_BASE
  jc near fail_stage
  mov [mod_stage_base], eax
.no_module_stage:
  sti
  call a20_enable
  jc near fail_a20
  call vbe_set                    ; режим экрана (не критично: без него ядро идёт в текстовом режиме/по серийному порту)
  call e820_collect
  jc near fail_e820
  ; область промежуточного файла ядра должна быть обычной памятью
  mov eax, STAGE_BASE
  mov ebx, [man_size]
  add ebx, 511
  and ebx, ~511
  add ebx, eax
  jc near fail_stage
  CONTAINED
  jc near fail_stage
  ; отдельный буфер initrd сразу за staging-областью ядра
  cmp dword [man_mod_size], 0
  je .no_module_ram_check
  mov eax, [mod_stage_base]
  mov ebx, [man_mod_size]
  add ebx, 511
  jc near fail_stage
  and ebx, ~511
  add ebx, eax
  jc near fail_stage
  CONTAINED
  jc near fail_stage
.no_module_ram_check:
  call load_kernel
  jc near fail_disk
  cmp dword [man_mod_size], 0
  je .module_load_done
  mov eax, [man_size]
  mov [kernel_size_saved], eax
  mov eax, [man_mod_lba]
  mov [man_lba], eax
  mov eax, [man_mod_size]
  mov [man_size], eax
  mov eax, [mod_stage_base]
  mov [load_base], eax
  call load_kernel
  jc near fail_module_disk
  mov eax, [kernel_size_saved]
  mov [man_size], eax
  mov dword [load_base], STAGE_BASE
  mov si, m_module_loaded
  call print16
.module_load_done:
  mov si, m_loaded
  call print16
  cli
  lgdt [gdtr]
  mov eax, cr0
  or al, 1
  mov cr0, eax
  jmp 0x10:pm_entry

serial_init:
  mov dx, 0x3F9
  xor al, al
  out dx, al
  mov dx, 0x3FB
  mov al, 0x80
  out dx, al
  mov dx, 0x3F8
  mov al, 1
  out dx, al
  inc dx
  xor al, al
  out dx, al
  mov dx, 0x3FB
  mov al, 3
  out dx, al
  mov dx, 0x3FA
  mov al, 0xC7
  out dx, al
  mov dx, 0x3FC
  mov al, 0x0B
  out dx, al
  ret

print16:                        ; DS:SI -> serial + экран BIOS
  lodsb
  test al, al
  jz .r
  SPUT
  mov ah, 0x0E
  mov bx, 7
  int 0x10
  jmp print16
.r: ret

a20_enable:
  call a20_test
  jnc .ok
  mov ax, 0x2401
  int 0x15
  call a20_test
  jnc .ok
  in al, 0x92
  test al, 2
  jnz .t
  or al, 2
  and al, 0xFE
  out 0x92, al
.t:
  mov cx, 0x4000
.w:
  call a20_test
  jnc .ok
  loop .w
  stc
  ret
.ok: clc
  ret

a20_test:                       ; CF=0, если A20 включена (нет заворота адреса)
  push ds
  push es
  push si
  push di
  xor ax, ax
  mov ds, ax
  dec ax
  mov es, ax
  mov si, 0x0600
  mov di, 0x0610
  mov al, [ds:si]
  mov ah, [es:di]
  push ax
  mov byte [ds:si], 0x00
  mov byte [es:di], 0xFF
  cmp byte [ds:si], 0xFF
  pop ax
  mov [ds:si], al
  mov [es:di], ah
  pop di
  pop si
  pop es
  pop ds
  je .off
  clc
  ret
.off: stc
  ret

; VBE: ищет линейный режим 32/24 бита из списка предпочтений и включает его. CF не влияет, при неудаче vbe_ok=0.
; Предпочтения: 1280x720, 1024x768, 800x600 (32 бита, затем 24). Режимы берутся из списка видеоадаптера.
vbe_set:
  pusha
  push fs
  mov byte [vbe_ok], 0
  mov dword [VBE_CTRL], 'VBE2'
  mov di, VBE_CTRL
  mov ax, 0x4F00
  int 0x10
  cmp ax, 0x004F
  jne .done
  cmp dword [VBE_CTRL], 'VESA'
  jne .done
  mov si, 0                       ; индекс предпочтения
.pref:
  cmp si, NPREF
  jae .done
  mov bx, [VBE_CTRL + 16]         ; VideoModePtr: смещение, сегмент
  mov ax, [VBE_CTRL + 18]
  mov fs, ax
  mov bp, bx
.mode:
  mov cx, [fs:bp]
  cmp cx, 0xFFFF
  je .nextpref
  add bp, 2
  push si
  push bp
  push fs
  mov ax, 0x4F01
  mov di, vbe_mi
  int 0x10
  pop fs
  pop bp
  pop si
  cmp ax, 0x004F
  jne .mode
  mov ax, [vbe_mi]
  and ax, 0x0091                  ; поддерживается, графика, линейный буфер
  cmp ax, 0x0091
  jne .mode
  mov ax, si
  shl ax, 2
  mov di, ax
  mov ax, [pref + di]
  cmp ax, [vbe_mi + 0x12]
  jne .mode
  mov ax, [pref + di + 2]
  cmp ax, [vbe_mi + 0x14]
  jne .mode
  cmp byte [vbe_mi + 0x19], 32
  jne .mode
  cmp byte [vbe_mi + 0x1B], 6     ; memory model: Direct Color
  jne .mode
  mov bx, cx
  or bx, 0x4000                   ; линейный буфер
  mov ax, 0x4F02
  int 0x10
  cmp ax, 0x004F
  jne .mode
  mov ax, [vbe_mi + 0x32]         ; LinBytesPerScanLine (VBE 3)
  test ax, ax
  jnz .p
  mov ax, [vbe_mi + 0x10]
.p:
  mov [vbe_pitch], ax
  cmp dword [vbe_mi + 0x28], 0
  je .nextpref
  mov byte [vbe_ok], 1
  jmp .done
.nextpref:
  inc si
  jmp .pref
.done:
  pop fs
  popa
  ret

; Заполняет MB2 info: тег типа 6 (карта памяти) и конечный тег. CF=1 - ошибка/пустая карта.
e820_collect:
  xor ebx, ebx
  mov dword [e_count], 0
  mov di, INFO + 24
.next:
  push di
  mov di, e820buf
  mov dword [e820buf + 20], 1
  mov eax, 0xE820
  mov edx, 0x534D4150
  mov ecx, 24
  int 0x15
  pop di
  jc .end
  cmp eax, 0x534D4150
  jne .bad
  cmp ecx, 20
  jb .skip
  test dword [e820buf + 20], 1
  jz .skip
  mov eax, [e820buf + 8]
  or eax, [e820buf + 12]
  jz .skip
  cmp dword [e_count], MAXE
  jae .end
  mov eax, [e820buf]
  mov [di], eax
  mov eax, [e820buf + 4]
  mov [di + 4], eax
  mov eax, [e820buf + 8]
  mov [di + 8], eax
  mov eax, [e820buf + 12]
  mov [di + 12], eax
  mov eax, [e820buf + 16]
  cmp eax, 1
  jb .res
  cmp eax, 5
  jbe .tk
.res: mov eax, 2
.tk: mov [di + 16], eax
  mov dword [di + 20], 0
  add di, 24
  inc dword [e_count]
.skip:
  test ebx, ebx
  jnz .next
.end:
  cmp dword [e_count], 0
  je .bad
  mov eax, [e_count]
  imul eax, 24
  add eax, 16
  mov dword [INFO + 8], 6
  mov [INFO + 12], eax
  mov dword [INFO + 16], 24
  mov dword [INFO + 20], 0
  add eax, 8
  mov edx, eax
  cmp byte [vbe_ok], 0
  je .nofb
  mov dword [INFO + edx], 8       ; тег framebuffer (тип 8), 38 байт данных -> 40 с выравниванием
  mov dword [INFO + edx + 4], 38
  mov eax, [vbe_mi + 0x28]
  mov [INFO + edx + 8], eax
  mov dword [INFO + edx + 12], 0
  movzx eax, word [vbe_pitch]
  mov [INFO + edx + 16], eax
  movzx eax, word [vbe_mi + 0x12]
  mov [INFO + edx + 20], eax
  movzx eax, word [vbe_mi + 0x14]
  mov [INFO + edx + 24], eax
  mov al, [vbe_mi + 0x19]
  mov [INFO + edx + 28], al
  mov byte [INFO + edx + 29], 1   ; прямые цвета RGB
  mov word [INFO + edx + 30], 0
  mov al, [vbe_mi + 0x20]
  mov [INFO + edx + 32], al
  mov al, [vbe_mi + 0x1F]
  mov [INFO + edx + 33], al
  mov al, [vbe_mi + 0x22]
  mov [INFO + edx + 34], al
  mov al, [vbe_mi + 0x21]
  mov [INFO + edx + 35], al
  mov al, [vbe_mi + 0x24]
  mov [INFO + edx + 36], al
  mov al, [vbe_mi + 0x23]
  mov [INFO + edx + 37], al
  mov dword [INFO + edx + 38], 0  ; выравнивание тега
  add edx, 40
.nofb:
  cmp byte [cmdbuf], 0            ; командная строка из образа (tools/mkbios.py, KIRA_CMDLINE) -> тег типа 1
  je .nocmd
  xor ecx, ecx
.cl:
  cmp byte [cmdbuf + ecx], 0
  je .cld
  inc ecx
  cmp ecx, 126
  jb .cl
.cld:
  lea eax, [ecx + 16]             ; 8 заголовок + строка + NUL, до кратного 8
  and eax, 0xFFFFFFF8
  push eax
  push ecx
  xor ebx, ebx                    ; тег чистится целиком: выравнивающие байты нулевые
.cz:
  mov dword [INFO + edx + ebx], 0
  add ebx, 4
  cmp ebx, eax
  jb .cz
  pop ecx
  pop eax
  mov dword [INFO + edx], 1
  lea ebx, [ecx + 9]
  mov [INFO + edx + 4], ebx
  xor ebx, ebx
.cc:
  mov al, [cmdbuf + ebx]
  mov [INFO + edx + 8 + ebx], al
  inc ebx
  cmp ebx, ecx
  jbe .cc
  lea eax, [ecx + 16]
  and eax, 0xFFFFFFF8
  add edx, eax
.nocmd:
  cmp dword [man_mod_size], 0
  je .nomodtag
  mov eax, [mod_stage_base]
  mov ebx, eax
  add ebx, [man_mod_size]
  jc .bad
  cmp ebx, 0xFFFFFFFF
  ja .bad
  mov dword [INFO + edx], 3       ; Multiboot2 module tag
  mov dword [INFO + edx + 4], 27  ; 16-byte header + "initrd.img" + NUL
  mov [INFO + edx + 8], eax
  mov [INFO + edx + 12], ebx
  mov byte [INFO + edx + 16], 'i'
  mov byte [INFO + edx + 17], 'n'
  mov byte [INFO + edx + 18], 'i'
  mov byte [INFO + edx + 19], 't'
  mov byte [INFO + edx + 20], 'r'
  mov byte [INFO + edx + 21], 'd'
  mov byte [INFO + edx + 22], '.'
  mov byte [INFO + edx + 23], 'i'
  mov byte [INFO + edx + 24], 'm'
  mov byte [INFO + edx + 25], 'g'
  mov byte [INFO + edx + 26], 0
  mov dword [INFO + edx + 27], 0 ; выравнивание до 32 байт
  mov byte [INFO + edx + 31], 0
  add edx, 32
.nomodtag:
  lea eax, [edx + 8]              ; + конечный тег
  mov [INFO], eax
  mov dword [INFO + 4], 0
  mov dword [INFO + edx], 0
  mov dword [INFO + edx + 4], 8
  clc
  ret
.bad: stc
  ret

; Загрузка с компакт-диска (El Torito без эмуляции): сектор диска 2048 байт, образ загрузчика лежит с адреса cd_base (его сообщает BIOS, INT 13h AH=4B01).
; LBA в манифесте и в dap - в секторах по 512 байт от начала образа; для компакт-диска переводятся в секторы по 2048 (все чтения кратны 4 по началу).
cd_detect:                      ; CF=1 - компакт-диск, но адрес образа не получен
  mov byte [is_cd], 0
  mov word [edd_buf], 0x1E
  mov si, edd_buf
  mov dl, [drive]
  mov ah, 0x48
  int 0x13
  jc .no
  cmp word [edd_buf + 24], 2048
  jne .no
  mov byte [spec], 0x13
  mov si, spec
  mov dl, [drive]
  mov ax, 0x4B01
  int 0x13
  jc .bad
  mov eax, [spec + 4]            ; пакет спецификации: +4 - LBA образа загрузки
  mov [cd_base], eax
  mov byte [is_cd], 1
.no: clc
  ret
.bad: stc
  ret

disk_read:                      ; читает [dap] (сектора по 512 от начала образа). CF=1 при ошибке
  cmp byte [is_cd], 0
  je disk_raw
  push dword [dap_lba]
  push word [dap_n]
  mov ax, [dap_n]
  add ax, 3
  shr ax, 2
  mov [dap_n], ax
  mov eax, [dap_lba]
  shr eax, 2
  add eax, [cd_base]
  mov [dap_lba], eax
  call disk_raw
  pop word [dap_n]
  pop dword [dap_lba]
  ret                           ; CF от disk_raw: pop не меняет флаги

disk_raw:
  mov cx, 3
.r:
  mov si, dap
  mov dl, [drive]
  mov ah, 0x42
  int 0x13
  jnc .ok
  xor ax, ax
  mov dl, [drive]
  int 0x13
  loop .r
  stc
  ret
.ok: clc
  ret

copy_high:                      ; ECX дворд из BOUNCE в линейный EDI (unreal mode)
  cli
  lgdt [gdtr]
  mov eax, cr0
  or al, 1
  mov cr0, eax
  jmp short $ + 2
  mov ax, 8
  mov es, ax
  mov eax, cr0
  and al, 0xFE
  mov cr0, eax
  mov ax, BOUNCE_SEG
  mov ds, ax
  xor esi, esi
  a32 rep movsd
  xor ax, ax
  mov ds, ax
  mov es, ax
  sti
  ret

load_kernel:
  mov eax, [man_lba]
  mov [cur_lba], eax
  mov eax, [man_size]
  mov [rem], eax
  mov eax, [load_base]
  mov [dest], eax
.loop:
  mov eax, [rem]
  test eax, eax
  jz .done
  add eax, 511
  shr eax, 9
  cmp eax, CHUNK
  jbe .c
  mov eax, CHUNK
.c:
  mov [chunk], ax
  mov [dap_n], ax
  mov eax, [cur_lba]
  mov [dap_lba], eax
  call disk_read
  jc .err
  movzx ecx, word [chunk]
  shl ecx, 7
  mov edi, [dest]
  call copy_high
  movzx eax, word [chunk]
  add [cur_lba], eax
  shl eax, 9
  add [dest], eax
  mov ecx, [rem]
  cmp ecx, eax
  jbe .z
  sub ecx, eax
  jmp .s
.z: xor ecx, ecx
.s: mov [rem], ecx
  jmp .loop
.done: clc
  ret
.err: stc
  ret

fail_manifest: mov si, m_manifest
  jmp fail16
fail_a20: mov si, m_a20
  jmp fail16
fail_e820: mov si, m_e820
  jmp fail16
fail_stage: mov si, m_stage
  jmp fail16
fail_disk: mov si, m_disk
  jmp fail16
fail_module_disk: mov si, m_module_disk
fail16:
  call print16
  cli
.h: hlt
  jmp .h

; ---------------------------------------------------------------- 32 бита
bits 32
pm_entry:
  mov ax, 8
  mov ds, ax
  mov es, ax
  mov fs, ax
  mov gs, ax
  mov ss, ax
  mov esp, 0x7C00
  cld
  ; CRC32 файла ядра
  mov esi, STAGE_BASE
  mov ecx, [man_size]
  mov edx, 0xFFFFFFFF
.cb:
  test ecx, ecx
  jz .cd
  movzx eax, byte [esi]
  xor dl, al
  mov ebx, 8
.cbit:
  shr edx, 1
  jnc .nc
  xor edx, 0xEDB88320
.nc:
  dec ebx
  jnz .cbit
  inc esi
  dec ecx
  jmp .cb
.cd:
  not edx
  cmp edx, [man_crc]
  mov esi, m_crc
  jne near fail32
  ; Initrd CRC32: не передаём ядру повреждённый модуль.
  cmp dword [man_mod_size], 0
  je .module_crc_ok
  mov esi, [mod_stage_base]
  mov ecx, [man_mod_size]
  mov edx, 0xFFFFFFFF
.mod_crc_byte:
  test ecx, ecx
  jz .mod_crc_done
  movzx eax, byte [esi]
  xor dl, al
  mov ebx, 8
.mod_crc_bit:
  shr edx, 1
  jnc .mod_crc_next
  xor edx, 0xEDB88320
.mod_crc_next:
  dec ebx
  jnz .mod_crc_bit
  inc esi
  dec ecx
  jmp .mod_crc_byte
.mod_crc_done:
  not edx
  cmp edx, [man_mod_crc]
  mov esi, m_mod_crc
  jne near fail32
.module_crc_ok:
  ; ELF64
  mov ebp, STAGE_BASE
  mov edi, [man_size]
  mov esi, m_elf
  cmp dword [ebp], 0x464C457F
  jne near fail32
  cmp byte [ebp + 4], 2
  jne near fail32
  cmp byte [ebp + 5], 1
  jne near fail32
  cmp word [ebp + 16], 2
  jne near fail32
  cmp word [ebp + 18], 62
  jne near fail32
  cmp word [ebp + 54], 56
  jne near fail32
  movzx ecx, word [ebp + 56]
  test ecx, ecx
  jz near fail32
  cmp ecx, 16
  ja near fail32
  cmp dword [ebp + 36], 0
  jne near fail32
  cmp dword [ebp + 28], 0
  jne near fail32
  mov eax, [ebp + 32]             ; e_phoff
  cmp eax, edi
  ja near fail32
  imul ebx, ecx, 56
  add ebx, eax
  jc near fail32
  cmp ebx, edi
  ja near fail32
  mov esi, m_seg
  lea ebx, [ebp + eax]            ; первый phdr
  mov [ph_cur], ebx
  mov [ph_cnt], ecx
  mov dword [n_load], 0
  ; проход 1: проверка всех PT_LOAD, затем проход 2: копирование
  mov edx, 0
.pass:
  mov ebx, [ph_cur]
  mov ecx, [ph_cnt]
.ph:
  test ecx, ecx
  jz .pdone
  cmp dword [ebx], 1
  jne .pn
  cmp dword [ebx + 12], 0         ; offset hi
  jne near fail32_seg
  cmp dword [ebx + 28], 0         ; paddr hi
  jne near fail32_seg
  cmp dword [ebx + 36], 0
  jne near fail32_seg
  cmp dword [ebx + 44], 0
  jne near fail32_seg
  mov eax, [ebx + 32]             ; filesz
  cmp eax, [ebx + 40]
  ja near fail32_seg
  mov edi, [man_size]
  mov esi, [ebx + 8]              ; offset
  cmp esi, edi
  ja near fail32_seg
  sub edi, esi
  cmp eax, edi
  ja near fail32_seg
  mov eax, [ebx + 24]             ; paddr
  cmp eax, 0x100000
  jb near fail32_seg
  mov edi, eax
  add edi, [ebx + 40]
  jc near fail32_seg
  cmp edi, STAGE_BASE
  ja near fail32_seg
  push ebx
  mov ebx, edi
  CONTAINED
  pop ebx
  jc near fail32_seg
  test edx, edx
  jz .pn
  ; копирование
  push ecx
  mov esi, [ebx + 8]
  add esi, STAGE_BASE
  mov edi, [ebx + 24]
  mov ecx, [ebx + 32]
  rep movsb
  mov ecx, [ebx + 40]
  sub ecx, [ebx + 32]
  xor eax, eax
  rep stosb
  pop ecx
.pn:
  add ebx, 56
  dec ecx
  jmp .ph
.pdone:
  test edx, edx
  jnz .go
  inc edx
  jmp .pass
.go:
  mov eax, [ebp + 24]              ; точка входа в загруженном сегменте
  cmp eax, 0x100000
  jb near fail32_entry
  cmp eax, STAGE_BASE
  jae near fail32_entry
  mov [entry], eax
  mov esi, m_go
  call print32
  mov ebx, INFO
  mov eax, 0x36D76289
  jmp dword [entry]

fail32_seg: mov esi, m_seg
  jmp fail32
fail32_entry: mov esi, m_entry
fail32:
  call print32
  cli
.h: hlt
  jmp .h

print32:
  lodsb
  test al, al
  jz .r
  SPUT
  jmp print32
.r: ret

; ---------------------------------------------------------------- данные
align 8
gdt:
  dq 0
  dq 0x00CF92000000FFFF          ; 0x08 data, 4 ГиБ
  dq 0x00CF9A000000FFFF          ; 0x10 code32
gdtr:
  dw gdtr - gdt - 1
  dd gdt

drive:   db 0
is_cd:   db 0
align 4
cd_base: dd 0
edd_buf: times 32 db 0
spec:    times 24 db 0
align 4
e_count: dd 0
cur_lba: dd 0
rem:     dd 0
dest:    dd 0
load_base: dd STAGE_BASE
mod_stage_base: dd 0
kernel_size_saved: dd 0
chunk:   dw 0
ph_cur:  dd 0
ph_cnt:  dd 0
n_load:  dd 0
entry:   dd 0
e820buf: times 24 db 0
vbe_ok:  db 0
align 4
vbe_pitch: dw 0
pref:    dw 1280, 720, 1024, 768, 800, 600, 640, 480
NPREF    equ 4
vbe_mi:  times 256 db 0
align 4
dap:
  db 0x10, 0
dap_n: dw 0
  dw 0x0000, BOUNCE_SEG
dap_lba: dq 0

m_banner:  db "KIRA BIOS stage2", 13, 10, 0
m_loaded:  db "KIRA: kernel image read", 13, 10, 0
m_module_loaded: db "KIRA: initrd image read", 13, 10, 0
m_go:      db "KIRA: entering kernel", 13, 10, 0
m_manifest: db "KIRA: bad manifest", 13, 10, 0
m_a20:     db "KIRA: A20 unavailable", 13, 10, 0
m_e820:    db "KIRA: E820 failed", 13, 10, 0
m_stage:   db "KIRA: no RAM for kernel/initrd staging", 13, 10, 0
m_disk:    db "KIRA: kernel read error", 13, 10, 0
m_module_disk: db "KIRA: initrd read error", 13, 10, 0
m_crc:     db "KIRA: kernel CRC mismatch", 13, 10, 0
m_mod_crc: db "KIRA: initrd CRC mismatch", 13, 10, 0
m_elf:     db "KIRA: bad ELF64 kernel", 13, 10, 0
m_seg:     db "KIRA: ELF segment outside usable RAM or malformed", 13, 10, 0
m_entry:   db "KIRA: bad kernel entry", 13, 10, 0

times (S2_BYTES - 32 - 128) - ($ - $$) db 0
cmdbuf:    times 128 db 0     ; командная строка загрузки (заполняет tools/mkbios.py), пусто - тега нет
man_magic: dd 0               ; 'KBM1'
man_lba:   dd 0
man_size:  dd 0
man_crc:   dd 0
man_mod_lba: dd 0
man_mod_size: dd 0
man_mod_crc: dd 0
man_flags: dd 0
