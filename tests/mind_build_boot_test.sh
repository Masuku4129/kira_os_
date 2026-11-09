#!/bin/bash
# Задание на сборку на настоящем ядре Киры в QEMU: команды вводятся через последовательную консоль (сценарий mind-build, ci/scenarios.py):
# build probe -> хеш в отчёте -> run без разрешения отклонён -> allow с неверным префиксом отклонён -> allow с верным -> run выполняется без прав
# и возвращает контрольную сумму, совпадающую с расчётом хоста; подмена образа между allow и run отклоняется; подделка разрешения в песочнице
# не действует. Нужен образ после `timeout 600 ./build_clang.sh --no-run </dev/null` (kira.iso, kira.disk). Без KVM работает на TCG (дольше).
set -u
cd "$(dirname "$0")/.."
[ -s kira.iso ] && [ -s kira.disk ] || { echo "FAIL: нет kira.iso/kira.disk (сначала: timeout 600 ./build_clang.sh --no-run </dev/null)"; exit 1; }
command -v qemu-system-x86_64 >/dev/null || { echo "FAIL: нет qemu-system-x86_64"; exit 1; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
python3 ci/kira_vm.py --scenario mind-build --out "$T" || { echo "mind build boot: FAIL; конец журнала консоли:"; tail -c 3000 "$T/mind-build/serial.log" 2>/dev/null; exit 1; }
echo "mind build boot: PASS"
