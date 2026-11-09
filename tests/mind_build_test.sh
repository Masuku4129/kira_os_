#!/bin/bash
# Задание на сборку (mind_build.c): шаблон -> квоты -> настоящий kcc -> elf_parse ядра -> песочница, разрешение человека или подписи,
# запуск только после mbj_check_grant под хост-загрузчиком tests/kcc/kloader. Подпись делает настоящий офлайн-инструмент tools/kdrvsign.py
# тестовым ключом (RFC 8032, тот же, что в ci/test_kdrvsign.py; он принимается только сборкой с KDRV_TEST_TRUST).
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
# Разум не имеет пути к разрешению и запуску: ни одно из его файлов не называет allow/run/запуск программ/песочницу /data, а вызовы
# выдачи разрешения и запуска образа есть только в buildcmd.c (команда человека), console.c (ввод) и в определяющих файлах.
for f in mind.c mind.h mindnet.c mindnet.h mind_sys.c mind_sys.h mind_sync.h mind_archive_policy.h; do
    if grep -nE 'mbj_authorize|mbj_check_grant|mbj_grants_|prog_run|build_cmd|proc_spawn|console_request|runlocal|/data' $f; then echo "FAIL: $f обращается к разрешению, запуску или песочнице"; exit 1; fi
done
callers=$(grep -lE 'mbj_authorize_|mbj_check_grant|mbj_grants_put|prog_run_local_image|build_cmd\(' *.c userland/*.c | sort | tr '\n' ' ')
[ "$callers" = "buildcmd.c console.c mind_build_grant.c progs.c " ] || { echo "FAIL: неожиданные места выдачи разрешения или запуска образа: $callers"; exit 1; }
echo "разрешение и запуск образа: пути из Разума нет (проверено по исходникам)"
$CC -std=gnu99 -O1 -g -Wall -Wextra -Werror -DKIRA_HOST_TEST -o $T/kloader tests/kcc/kloader.c elf.c
$CC -std=gnu99 -O1 -g -Wall -Wextra -Werror -DKIRA_HOST_TEST -DKIRA_TEST_BUILD -DKDRV_TEST_TRUST -I. -Iarch/x86_64 \
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -o $T/mbt \
    tests/mind_build_test.c mind_build.c mind_build_core.c mind_build_grant.c mind.c mindnet.c elf.c sha256.c kdrv_sig.c pk.c crypto.c \
    $(ls kcc/*.c | grep -v '^kcc/main.c$')
mkdir -p $T/w
$T/mbt stage1 $T/w
python3 - "$T" <<'PY'
import sys
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, NoEncryption, PrivateFormat, PublicFormat
t = Path(sys.argv[1])
k = Ed25519PrivateKey.from_private_bytes(bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"))
(t / "owner.pem").write_bytes(k.private_bytes(Encoding.PEM, PrivateFormat.PKCS8, NoEncryption()))
(t / "owner.hex").write_text(k.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw).hex() + "\n")
PY
python3 tools/kdrvsign.py $T/w/cand.elf --key $T/owner.pem --public-key $T/owner.hex --trust-header kdrv_trust.h \
    --key-id TEST-ED25519-001 --not-before 1700000000 --not-after 4102444800 --output $T/w/cand.sig >/dev/null
$T/mbt stage2 $T/w $T/kloader
