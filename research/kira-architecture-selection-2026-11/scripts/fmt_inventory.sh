#!/bin/bash
# [S]+[E] Инвентаризация реальных форматов KIRA: что лежит на диске, каким форматом, чем описано. Вывод -> logs/fmt_inventory.log
. "$(dirname "$0")/fmt_env.sh"; cd "$FMT_ART" || exit 1
exec > "$FMT_LOGS/fmt_inventory.log" 2>&1
echo "== commit артефактов: $(git -C $FMT_ART log --oneline -1)  (клон исследования: $(git -C $FMT_REPO log --oneline -1))"
echo "== file(1) по артефактам"; file kira.bin kira.iso kira-bios.img kira-uefi.img kira-arm64.elf kira-arm64.img bios-stage1.bin bios-stage2.bin userland/fetch.elf userland/edit.elf userland/drvedu.elf userland/boot.o boot.o sha256.o 2>&1
echo; echo "== ET_REL (объект) vs ET_EXEC (исполняемый): readelf -h Type/Machine/Entry"
for f in sha256.o boot.o userland/edit.o userland/edit.elf kira.bin kira-arm64.elf build-arm64/elf.o; do [ -f $f ] && { echo "-- $f"; readelf -h $f | grep -E 'Type|Machine|Entry|Number of (program|section)'; }; done
echo; echo "== сегменты ядра x86-64 (kira.bin)"; readelf -lW kira.bin | sed -n 1,25p
echo; echo "== секции ядра"; readelf -SW kira.bin | head -40
echo; echo "== сегменты userland/edit.elf (user.ld, база 0x8000000000)"; readelf -lW userland/edit.elf
echo; echo "== сегменты ARM64 ядра (static-pie + .rela)"; readelf -lW kira-arm64.elf | head -20
echo; echo "== отладочная информация: DWARF-секции в ядре/userland/ARM64"
for f in kira.bin userland/edit.elf userland/fetch.elf kira-arm64.elf; do echo "-- $f: .debug_*=$(readelf -SW $f | grep -c '\.debug_') .symtab=$(readelf -SW $f | grep -c '\.symtab') .eh_frame=$(readelf -SW $f | grep -c '\.eh_frame') .note=$(readelf -SW $f | grep -c '\.note')"; done
echo; echo "== символы: число в kira.bin (nm) / userland/fetch.elf"; nm kira.bin | wc -l; nm userland/fetch.elf | wc -l
echo; echo "== флаги -g в сборке (build_clang.sh / build_arm64.sh):"; grep -c -- ' -g' build_clang.sh build_arm64.sh
echo; echo "== архивы .a: поддержка в klink/kcc/сборке"; grep -n '"!<arch>' klink/klink.c userland/drvget_core.c | cut -c1-200; ls *.a userland/*.a 2>&1 | head -2; grep -n "ar \|ar$\|llvm-ar" build_clang.sh | head -3
echo; echo "== падения: crashdump.c (текстовый дамп регистров, не core/minidump)"; grep -n 'minidump\|core file\|coredump' -ri *.c *.h docs/*.md 2>/dev/null | head -5 | cut -c1-200
echo; echo "== BIOS-образ: MBR + stage1/stage2 + ядро ELF (mkbios.py)"; sed -n 1,25p tools/mkbios.py | cut -c1-200
od -A d -t x1 -N 16 bios-stage1.bin; od -A d -t x1 -j 510 -N 2 bios-stage1.bin; ls -l bios-stage1.bin bios-stage2.bin kira-bios.img
echo; echo "== UEFI-образ: GPT+ESP, BOOTX64.EFI (PE32+) и KIRA.ELF"; sed -n 1,15p tools/mkuefi.py | cut -c1-200
mdir -i kira-uefi.img@@1M -/ ::/ 2>&1 | head -20 || true
echo; echo "== ARM64 Image заголовок (первые 64 байта) и magic по смещению 56"; od -A d -t x1z -N 64 kira-arm64.img
echo; echo "== kdrv-пакет: kdrv_header_t=136 Б + ELF; .rec=216 Б; .sig=144 Б (kdrv_sig_t) -- static_assert в заголовках"; grep -n '_Static_assert' kdrv_format.h kdrv_sig.h bootinfo.h | cut -c1-140
echo; echo "== kfs-образ диска (kira.disk): суперблок"; od -A d -t x1z -N 64 kira.disk
echo; echo "== Манифест доверенных программ (manifest.h): генерируется kfstool manifest"; head -c 600 build-arm64/manifest.h 2>/dev/null; echo
echo; echo "== syscall x86-64 (interrupts.s:336): номер rax, аргументы rdi,rsi,rdx (3 арг.), rcx/r11 затираются SYSCALL, r8-r10 обнуляются на выходе"
echo "== syscall AArch64 (arch/aarch64/main.c user_exception): SVC, номер x8, аргументы x0..x2, результат x0"
echo "== ABI-документация: grep 'ABI' в docs/ARCHITECTURE:"; grep -c 'ABI' ARCHITECTURE.md docs/*.md | grep -v ':0'
echo "== число SYS_* в syscall.c:"; grep -c '^#define SYS_' syscall.c
