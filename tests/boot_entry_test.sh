#!/bin/bash
# Точка входа ядра одна, а входят в неё и 32-bit (GRUB, BIOS stage2), и 64-bit (UEFI) режимах.
# Проверяем по дизассемблированию, что первые байты безопасно разводят пути в обоих режимах:
# а не превращаются в 64-битном режиме в запись по случайному адресу.
set -e
cd "$(dirname "$0")/.."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
nasm -f elf64 arch/x86_64/boot.s -o "$T/boot.o"

d64=$(objdump -d -M intel -j .text "$T/boot.o" | sed -n '/<start>:/,/<start.protected_entry>:/p')
d32=$(objdump -d -M intel,i386 -j .text "$T/boot.o" | sed -n '/<start>:/,/<start.protected_entry>:/p')

fail() { echo "boot_entry FAIL: $1"; echo "--- 64-bit decode ---"; echo "$d64"; echo "--- 32-bit decode ---"; echo "$d32"; exit 1; }

# 64-bit (UEFI): никаких обращений к памяти до перехода на .uefi_entry, RCX/RDX не затронуты.
echo "$d64" | grep -q "movabs r14" || fail "64-bit entry must start with the harmless movabs r14"
echo "$d64" | grep -q "jmp .*<start.uefi_entry>" || fail "64-bit entry must reach .uefi_entry"
echo "$d64" | grep -Eq "\[r?(ip|[a-d]x|[sd]i|[sb]p)|ds:|rcx|rdx|ecx,|edx," && \
    fail "64-bit entry prologue touches memory or RCX/RDX before .uefi_entry"

# 32-bit (Multiboot2/BIOS): EAX/EBX сохраняются позже, пролог их не трогает и прыгает на .protected_entry.
echo "$d32" | grep -q "dec *ecx" || fail "32-bit prologue must decode as dec ecx"
echo "$d32" | grep -q "mov *esi,0x" || fail "32-bit prologue must decode as mov esi, imm32"
echo "$d32" | grep -q "jmp .*<start.protected_entry>" || fail "32-bit entry must reach .protected_entry"
echo "$d32" | grep -Eq "\b(eax|ebx)\b" && fail "32-bit prologue must not touch EAX/EBX (Multiboot2 magic/info)"

echo "boot_entry: PASS"
