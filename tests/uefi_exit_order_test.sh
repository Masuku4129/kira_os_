#!/bin/bash
# Между GetMemoryMap и ExitBootServices нельзя вызывать ничего, что обращается к Boot Services:
# вывод на консоль (Print, debug_marker), выделение памяти и т.п. меняют карту памяти, ключ карты
# устаревает, и ExitBootServices возвращает EFI_INVALID_PARAMETER при каждой попытке.
set -e
cd "$(dirname "$0")/.."
python3 -I - "${1:-arch/x86_64/uefi/loader.c}" <<'PY'
import re, sys
src = open(sys.argv[1]).read()
exit_at = src.index("g_bs->ExitBootServices")
head = src[:exit_at]
gm = head.rindex("g_bs->GetMemoryMap")
region = head[gm:]
# Ветка повторной попытки при EFI_BUFFER_TOO_SMALL выполняется до нового GetMemoryMap и разрешена.
region = re.sub(r"if \(status == EFI_BUFFER_TOO_SMALL\) \{.*?continue;\s*\}", "", region, flags=re.S)
# Ветка отказа kira_uefi_build_kbi завершает загрузку (return), на ExitBootServices не попадает.
region = re.sub(r"if \(EFI_ERROR\(status\)\) \{\s*Print\(.*?return status;\s*\}", "", region, flags=re.S)
bad = [m.group(0) for m in re.finditer(r"\b(Print|debug_marker|uefi_call_wrapper\(g_bs->(?!GetMemoryMap)\w+)", region)]
if bad:
    print("uefi_exit_order FAIL: boot services used between GetMemoryMap and ExitBootServices:", bad)
    sys.exit(1)
print("uefi_exit_order: PASS")
PY
