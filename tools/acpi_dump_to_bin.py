#!/usr/bin/env python3
"""Из журнала ОС (команда "acpi dump dsdt" / "acpi dump ssdtN") достаёт таблицы ACPI в двоичные файлы.
Строки журнала: "ACPIDUMP BEGIN DSDT 4116", затем шестнадцатеричные строки, затем "ACPIDUMP END DSDT".
Использование: acpi_dump_to_bin.py журнал.txt каталог_вывода [префикс]
Проверяются длина и контрольная сумма таблицы (сумма всех байт = 0 по модулю 256); при расхождении выход с ошибкой."""
import re
import sys


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    text = open(sys.argv[1], "rb").read().decode("latin-1")
    outdir = sys.argv[2]
    prefix = sys.argv[3] if len(sys.argv) > 3 else ""
    found = 0
    for m in re.finditer(r"ACPIDUMP BEGIN (\S+) (\d+)\r?\n(.*?)ACPIDUMP END \1", text, re.S):
        name, ln, body = m.group(1), int(m.group(2)), m.group(3)
        hexs = "".join(l.strip() for l in body.splitlines() if re.fullmatch(r"[0-9a-f]+", l.strip()))
        data = bytes.fromhex(hexs)
        if len(data) != ln:
            print("ошибка: %s: длина %d, ожидалась %d" % (name, len(data), ln))
            return 1
        if sum(data) & 0xFF:
            print("ошибка: %s: контрольная сумма не сходится" % name)
            return 1
        path = "%s/%s%s.aml" % (outdir, prefix, name.lower())
        open(path, "wb").write(data)
        print("%s: %d байт -> %s" % (name, ln, path))
        found += 1
    if not found:
        print("в журнале нет выгрузки таблиц")
        return 1
    return 0


sys.exit(main())
