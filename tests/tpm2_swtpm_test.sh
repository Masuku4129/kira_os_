#!/bin/bash
# Команды tpm2.c на настоящем swtpm (TPM 2.0): определение, счётчик, обычная ячейка. Без swtpm проверка пропускается с причиной.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
if ! command -v swtpm >/dev/null; then echo "swtpm не найден: проверка на настоящем эмуляторе TPM пропущена (apt install swtpm)"; exit 0; fi
T=$(mktemp -d); trap 'kill $PID 2>/dev/null; rm -rf $T' EXIT
$CC -O1 -g -Wall -Werror -I. -o $T/t tests/tpm2_swtpm_test.c tpm2.c rollback.c
PORT=$((20000 + RANDOM % 20000))
swtpm socket --tpm2 --tpmstate dir=$T --server type=tcp,port=$PORT --ctrl type=tcp,port=$((PORT+1)) --flags not-need-init,startup-clear >$T/swtpm.log 2>&1 &
PID=$!
for i in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break; sleep 0.1; done
$T/t $PORT
