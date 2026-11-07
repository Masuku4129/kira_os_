#!/bin/bash
# drvget целиком на хосте: настоящий drvget.elf (x86-64, собран теми же флагами, что для Киры) работает под загрузчиком tests/kcc/kloader
# (системные вызовы Киры -> файлы и сокеты хоста), скачивает рецепт и исходники по настоящему TLS 1.3 с локального сервера ci/https_once.py
# (цепочка ci/mkchain.py, корневой - единственный доверенный), собирает драйвер встроенным kcc и пишет /drv. Не заменяет стенд: ядро Киры,
# его проверки FS_OP_DRV_* и загрузка драйвера drvd проверяются на QEMU. Использование: tests/drvget_e2e.sh [каталог-сборки]
set -u
cd "$(dirname "$0")/.."
T=${1:-$(mktemp -d)}
mkdir -p $T
fail=0
note() { echo "FAIL: $*"; fail=$((fail+1)); }
command -v clang >/dev/null && command -v ld.lld >/dev/null && command -v openssl >/dev/null && python3 -c "import cryptography" 2>/dev/null || { echo "drvget e2e: нужны clang, ld.lld, openssl и python3-cryptography - пропущено"; exit 0; }
CC=clang; K="$CC --target=x86_64-unknown-none-elf"
UCFLAGS="-ffreestanding -fPIE -fvisibility=hidden -mno-red-zone -mno-sse -mno-mmx -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -Wall -Wextra -Werror"
$K $UCFLAGS -c -x c /dev/null -o $T/probe.o 2>/dev/null || { echo "drvget e2e: нет цели x86_64-unknown-none-elf у clang - пропущено"; exit 0; }
O=$T/o; mkdir -p $O
objs=""
for src in kcc/*.c; do
    [ $src = kcc/main.c ] && continue
    $K $UCFLAGS -nostdlibinc -Iuserland/kccrt -c $src -o $O/kcc_$(basename $src .c).o || exit 1
    objs="$objs $O/kcc_$(basename $src .c).o"
done
$K $UCFLAGS -nostdlibinc -Iuserland -c userland/kccrt.c -o $O/kccrt.o || exit 1
$K $UCFLAGS -c userland/libc_min.c -o $O/libc_min.o || exit 1
$K $UCFLAGS -DKIRA_TEST_BUILD -DKDRV_TEST_TRUST -nostdlibinc -I. -Iuserland -Iuserland/kccrt -c userland/drvget.c -o $O/drvget.o || exit 1
$K $UCFLAGS -I. -c userland/drvget_core.c -o $O/dgcore.o || exit 1
$K $UCFLAGS -I. -c kdrv_fmt.c -o $O/kfmt.o || exit 1
for f in userland/tls.c userland/x509.c pk.c crypto.c sha256.c kdrv_sig.c; do $K $UCFLAGS -I. -Iuserland -c $f -o $O/t_$(basename $f .c).o || exit 1; done
ld.lld -T userland/user.ld -z max-page-size=4096 -z noseparate-code -z norelro -o $T/drvget.elf $O/drvget.o $O/dgcore.o $O/kfmt.o $objs $O/kccrt.o $O/t_*.o $O/libc_min.o || exit 1
# drvman (окно «Драйверы»): та же логика (drvget.c с -DDRVGET_LIB) + окно; здесь проверяется режим do под загрузчиком (вызовов окна и списка PCI у загрузчика нет)
$K $UCFLAGS -DKIRA_TEST_BUILD -DKDRV_TEST_TRUST -nostdlibinc -I. -Iuserland -Iuserland/kccrt -DDRVGET_LIB -c userland/drvget.c -o $O/drvget_lib.o || exit 1
$K $UCFLAGS -nostdlibinc -I. -Iuserland -Iuserland/kccrt -c userland/drvman.c -o $O/drvman.o || exit 1
$K $UCFLAGS -I. -c font8x16.c -o $O/font.o || exit 1
ld.lld -T userland/user.ld -z max-page-size=4096 -z noseparate-code -z norelro -o $T/drvman.elf $O/drvman.o $O/drvget_lib.o $O/dgcore.o $O/kfmt.o $O/font.o $objs $O/kccrt.o $O/t_*.o $O/libc_min.o || exit 1
gcc -std=gnu99 -O1 -DKIRA_HOST_TEST -o $T/kloader tests/kcc/kloader.c elf.c || exit 1
gcc -O1 -DKIRA_HOST_TEST -I. -Iarch/x86_64 -o $T/dgcheck tests/drvget_check.c kdrv_fmt.c sha256.c elf.c || exit 1

# сервер и цепочка
python3 -I ci/mkchain.py $T/chain >/dev/null || exit 1
mkdir -p $T/ssl && cp $T/chain/root.der $T/ssl/roots.der
W=$T/www; rm -rf $W; mkdir -p $W/rl $W/gpl $W/bin $W/pin $W/nolic $W/winsys $W/nohash
cp drivers/rl/drvget.rec drivers/rl/rl.c drivers/rl/if_rlreg.h $W/rl/
# GPL: тот же драйвер, но в одном файле есть заголовок GPL
cp drivers/rl/* $W/gpl/; sed -i 's/SPDX-License-Identifier: BSD-3-Clause/SPDX-License-Identifier: GPL-2.0-only/' $W/gpl/if_rlreg.h; sed -i 's/^name rl/name gpl/' $W/gpl/drvget.rec; H=$(sha256sum $W/gpl/if_rlreg.h | awk '{print $1}'); sed -i "s#^file if_rlreg.h if_rlreg.h sha256 .*#file if_rlreg.h if_rlreg.h sha256 $H#" $W/gpl/drvget.rec
# двойная лицензия с GPL в комментарии
cp drivers/rl/* $W/nolic/; sed -i 's/^name rl/name nolic/' $W/nolic/drvget.rec; sed -i '1,40d' $W/nolic/if_rlreg.h; sed -i '1i /* без сведений о лицензии */' $W/nolic/if_rlreg.h; H=$(sha256sum $W/nolic/if_rlreg.h | awk '{print $1}'); sed -i "s#^file if_rlreg.h if_rlreg.h sha256 .*#file if_rlreg.h if_rlreg.h sha256 $H#" $W/nolic/drvget.rec
# бинарник, замаскированный под .c (Windows PE)
cp drivers/rl/* $W/bin/; sed -i 's/^name rl/name bin/' $W/bin/drvget.rec; printf 'MZ\220\0\3\0\0\0\4\0\0\0\377\377\0\0This program cannot be run in DOS mode.' > $W/bin/if_rlreg.h; H=$(sha256sum $W/bin/if_rlreg.h | awk '{print $1}'); sed -i "s#^file if_rlreg.h if_rlreg.h sha256 .*#file if_rlreg.h if_rlreg.h sha256 $H#" $W/bin/drvget.rec
# закреплённый хеш не совпадает
cp drivers/rl/* $W/pin/; sed -i 's/^name rl/name pin/' $W/pin/drvget.rec; sed -i 's#^file rl.c rl.c sha256 .*#file rl.c rl.c sha256 0000000000000000000000000000000000000000000000000000000000000001#' $W/pin/drvget.rec
# рецепт без SHA-256 исходников: установка обязана отказать
cp drivers/rl/* $W/nohash/; sed -i 's/^name rl/name nohash/' $W/nohash/drvget.rec; sed -i 's/ sha256 [0-9a-f]\{64\}$//' $W/nohash/drvget.rec
# рецепт просит файл .sys
cp drivers/rl/* $W/winsys/; sed -i 's/^name rl/name winsys/' $W/winsys/drvget.rec; echo 'file rtl8139.sys rtl8139.sys' >> $W/winsys/drvget.rec
PORT=$((20000 + $$ % 20000))
python3 -I ci/https_once.py --port $PORT --loop --chain $T/chain --serve $W >$T/srv.log 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
for i in $(seq 1 50); do grep -q ready $T/srv.log && break; sleep 0.1; done
grep -q ready $T/srv.log || { echo "drvget e2e: сервер не стартовал"; cat $T/srv.log; exit 1; }
U=https://127.0.0.1:$PORT
REC_SHA=$(sha256sum $W/rl/drvget.rec | awk '{print $1}')
printf 'kira-driver-index 1\npci 10ec:8139 rl %s/rl/drvget.rec %s\n' "$U" "$REC_SHA" > $T/ssl/drivers.index

run() { # аргументы drvget -> $T/out
    rm -f $T/out; timeout 120 $T/kloader -s $T/sb -i kcc/include -y $T/ssl -d $T/drv -a "$1" $T/drvget.elf >$T/out 2>&1; rc=$?
}
rm -rf $T/sb $T/drv; mkdir -p $T/sb $T/drv

# 1. PCI-подбор и каталог: рецепт закреплён SHA-256; без confirm пакет не устанавливается
run "match 10ec:8139"
grep -q "drvget match 10ec:8139: rl" $T/out || note "match: $(tail -c 300 $T/out)"
run "match ffff:ffff"
grep -q "подходящий рецепт.*не найден" $T/out || note "match: неизвестный PCI ID не отвергнут"
run "install $U/rl/drvget.rec"
grep -q "SHA-256 рецепта совпал с системным каталогом" $T/out || note "install: recipe hash не сверён с каталогом"
cp $T/ssl/drivers.index $T/ssl/drivers.index.good
printf "broken catalogue\\n" > $T/ssl/drivers.index
run "install $U/rl/drvget.rec"
grep -q "системный каталог повреждён; установка заблокирована" $T/out || note "повреждённый системный каталог не заблокировал установку"
mv $T/ssl/drivers.index.good $T/ssl/drivers.index
[ -z "$(ls $T/sb | grep '^s-')" ] || note "повреждённый каталог оставил старые staging-файлы"
run "install $U/rl/drvget.rec"
grep -q "SHA-256 рецепта совпал с системным каталогом" $T/out || note "install после восстановления каталога не прошёл"
run "install $U/nohash/drvget.rec"
grep -q "не закреплён SHA-256" $T/out || note "рецепт без SHA-256 не отклонён"
[ -z "$(ls $T/sb | grep '^s-')" ] || note "неверный рецепт оставил старые staging-файлы"
run "install $U/rl/drvget.rec"
grep -q "SHA-256 рецепта совпал с системным каталогом" $T/out || note "повторная установка после отказа не прошла"
grep -q "Установка НЕ выполнена" $T/out || note "install: нет отчёта: $(tail -c 300 $T/out)"
grep -q "лицензия:   заявлена BSD-3-Clause" $T/out || note "install: нет лицензии"
grep -q "if_rlreg.h" $T/out && grep -q "10ec:8139" $T/out || note "install: нет файлов или PCI-ID"
grep -q "CAP_DMA" $T/out && grep -q "CAP_NETDEV" $T/out && grep -q "IOMMU" $T/out || note "install: нет списка прав или предупреждения про DMA"
grep -q "источник:   $U/rl/drvget.rec" $T/out || note "install: нет источника"
[ -z "$(ls $T/drv)" ] || note "install: что-то записано в /drv до подтверждения: $(ls $T/drv)"
HASH=$(sed -n 's/^SHA-256 набора исходников (рецепт + файлы): \([0-9a-f]\{64\}\)$/\1/p' $T/out)
[ ${#HASH} = 64 ] || note "install: нет хеша набора"
# хеш набора независимо: python считает так же, как dg_set_add
EXP=$(python3 -I - $W/rl <<'PY'
import hashlib, struct, sys
d = sys.argv[1]
h = hashlib.sha256()
def add(n, b): h.update(n.encode() + b"\0" + struct.pack("<Q", len(b)) + b)
add("drvget.rec", open(d + "/drvget.rec", "rb").read())
for n in ("rl.c", "if_rlreg.h"): add(n, open(d + "/" + n, "rb").read())
print(h.hexdigest())
PY
)
[ "$HASH" = "$EXP" ] || note "хеш набора $HASH не равен независимо посчитанному $EXP"

# 2. confirm с неверным хешем - отказ; ничего не записано
run "confirm rl 00000000"
grep -q "хеш не совпал" $T/out || note "confirm с чужим хешем не отклонён: $(tail -c 200 $T/out)"
[ -z "$(ls $T/drv)" ] || note "confirm с чужим хешем записал файлы"
run "confirm rl abc"
grep -q "хеш не совпал" $T/out || note "короткий хеш принят"

# 3. confirm с верным хешем: сборка на устройстве и запись
run "confirm rl ${HASH:0:8}"
grep -q "собрано, образ ELF" $T/out || note "confirm: сборка не прошла: $(tail -c 600 $T/out)"
grep -q "установлено: /drv/rl.kdrv и /drv/rl.rec" $T/out || note "confirm: не установлено: $(tail -c 300 $T/out)"
chk=$($T/dgcheck $T/drv rl 1  2>&1)  # заведомо неверные права: проверка самой проверки
echo "$chk" | grep -q "BAD caps" || note "dgcheck не заметил неверные права: $chk"
chk=$($T/dgcheck $T/drv rl $(python3 -I - <<'PY'
import re
s = open('caps.h', encoding='utf-8').read()
v = {}
for m in re.finditer(r'(CAP_[A-Z_]+)\s*=\s*(1u?\s*<<\s*(\d+)|0x[0-9a-fA-F]+)', s):
    v[m.group(1)] = (1 << int(m.group(3))) if m.group(3) else int(m.group(2), 16)
print('%x' % (v['CAP_MMIO'] | v['CAP_DMA'] | v['CAP_PCIDEV'] | v['CAP_NETDEV']))
PY
) 2>&1)
echo "$chk" | grep -q "^OK rl v1" || note "пакет в /drv неверен: $chk"
echo "$chk" | grep -q "source=$U/rl/drvget.rec" || note "в .rec нет источника"
run "list"
grep -q "rl: версия 1, права: MMIO DMA PCIDEV NETDEV, SHA-256 сверен" $T/out || note "list: $(tail -c 300 $T/out)"
run "verify rl"
grep -q "подпись издателя: отсутствует; пакет только локально подтверждён" $T/out || note "verify: локальный пакет не помечен как неподписанный"
grep -q "verify rl: OK" $T/out || note "verify: $(tail -c 300 $T/out)"
# испорченный образ: verify обязан заметить
cp $T/drv/rl.kdrv $T/rl.keep; printf '\377' | dd of=$T/drv/rl.kdrv bs=1 seek=200 conv=notrunc 2>/dev/null
run "verify rl"
grep -q "ОШИБКА" $T/out || note "verify не заметил испорченный пакет"
cp $T/rl.keep $T/drv/rl.kdrv
cp "$T/drv/rl.kdrv" "$T/rl.package" || note "cannot preserve deterministic package for publisher-signature E2E"
run "remove rl"
grep -q "удалён" $T/out && [ ! -e $T/drv/rl.kdrv ] || note "remove: $(tail -c 200 $T/out)"
# The host loader models file syscalls, not fsrv's companion cleanup policy.
rm -f $T/drv/rl.rec $T/drv/rl.sig

# 3a. Valid publisher signature over the exact package is fetched through TLS and installed.
python3 -I - "$T" <<'PY'
import sys
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, NoEncryption, PrivateFormat, PublicFormat
root = Path(sys.argv[1])
seed = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
key = Ed25519PrivateKey.from_private_bytes(seed)
(root / "test-key.pem").write_bytes(key.private_bytes(Encoding.PEM, PrivateFormat.PKCS8, NoEncryption()))
pub = key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
(root / "test-public.hex").write_text(pub.hex() + "\n", encoding="ascii")
(root / "test-trust.h").write_text(
    '#define KDRV_TRUST_KEY_ID "TEST-ED25519-001"\n'
    + "static const unsigned char test_pub[] = {"
    + ",".join(f"0x{x:02x}" for x in pub) + "};\n",
    encoding="ascii")
PY
python3 -I tools/kdrvsign.py "$T/rl.package" --key "$T/test-key.pem" \
    --public-key "$T/test-public.hex" --trust-header "$T/test-trust.h" \
    --key-id TEST-ED25519-001 --not-before 1700000000 --not-after 4102444800 \
    --output "$W/rl/drvget.sig" >"$T/sign.log" 2>&1 || note "test publisher signer failed: $(cat "$T/sign.log")"
rm -rf "$T/sb" "$T/drv"; mkdir -p "$T/sb" "$T/drv"
run "install $U/rl/drvget.rec"
HASH=$(sed -n 's/^SHA-256 набора исходников (рецепт + файлы): \([0-9a-f]\{64\}\)$/\1/p' "$T/out")
[ ${#HASH} = 64 ] || note "valid signature test: не получен хеш staging-набора"
run "confirm rl ${HASH:0:8}"
grep -q "подпись издателя Ed25519 подтверждена" "$T/out" || note "valid publisher signature was not accepted: $(tail -c 400 "$T/out")"
[ -s "$T/drv/rl.sig" ] || note "valid publisher signature was not installed beside package"
run "verify rl"
grep -q "Ed25519 подтверждена" "$T/out" || note "installed publisher signature did not verify: $(tail -c 400 "$T/out")"
# The record marks this package as publisher-authenticated; deleting .sig must not downgrade it to local.
cp "$T/drv/rl.sig" "$T/rl.sig.keep"
rm -f "$T/drv/rl.sig"
run "verify rl"
grep -q "подпись издателя: ОТКЛОНЕНА" "$T/out" && grep -q "verify rl: ОШИБКА" "$T/out" || note "missing mandatory signature was accepted as local: $(tail -c 400 "$T/out")"
cp "$T/rl.sig.keep" "$T/drv/rl.sig"
run "verify rl"
grep -q "verify rl: OK" "$T/out" || note "restored publisher signature did not verify: $(tail -c 300 "$T/out")"
rm -f "$W/rl/drvget.sig"

# A trusted catalogue entry can require publisher authentication: HTTP 404 must not downgrade to local trust.
printf 'kira-driver-index 1\npci 10ec:8139 rl %s/rl/drvget.rec %s publisher-required\n' "$U" "$REC_SHA" > "$T/ssl/drivers.index"
run "install $U/rl/drvget.rec"
HASH=$(sed -n 's/^SHA-256 набора исходников (рецепт + файлы): \([0-9a-f]\{64\}\)$/\1/p' "$T/out")
[ ${#HASH} = 64 ] || note "required publisher policy test: не получен хеш staging-набора"
run "confirm rl ${HASH:0:8}"
grep -q "системный каталог требует издательскую подпись, но сервер вернул HTTP 404" "$T/out" || note "required publisher policy allowed unsigned fallback: $(tail -c 400 "$T/out")"
[ -s "$T/drv/rl.sig" ] && [ -s "$T/drv/rl.kdrv" ] || note "required publisher policy damaged the installed signed package"
[ -z "$(ls "$T/sb" | grep '^s-')" ] || note "required publisher refusal left staging files"

# Without the required bit, a local 404 is still explicit, but cannot replace an existing signed package.
printf 'kira-driver-index 1\npci 10ec:8139 rl %s/rl/drvget.rec %s\n' "$U" "$REC_SHA" > "$T/ssl/drivers.index"
run "install $U/rl/drvget.rec"
HASH=$(sed -n 's/^SHA-256 набора исходников (рецепт + файлы): \([0-9a-f]\{64\}\)$/\1/p' "$T/out")
[ ${#HASH} = 64 ] || note "unsigned replacement test: не получен хеш staging-набора"
run "confirm rl ${HASH:0:8}"
grep -q "замена существующего подписанного пакета на unsigned/local запрещена" "$T/out" || note "signed package was silently downgraded: $(tail -c 400 "$T/out")"
[ -s "$T/drv/rl.sig" ] && [ -s "$T/drv/rl.kdrv" ] || note "refused downgrade damaged the existing signed package"
[ -z "$(ls "$T/sb" | grep '^s-')" ] || note "refused downgrade left staging files"
run "remove rl"
[ ! -e "$T/drv/rl.kdrv" ] || note "remove did not delete signed package"
rm -f "$T/drv/rl.rec" "$T/drv/rl.sig"

# 3b. A malformed present signature must abort instead of silently falling back to local trust.
rm -rf "$T/sb" "$T/drv"; mkdir -p "$T/sb" "$T/drv"
run "install $U/rl/drvget.rec"
HASH=$(sed -n 's/^SHA-256 набора исходников (рецепт + файлы): \([0-9a-f]\{64\}\)$/\1/p' $T/out)
[ ${#HASH} = 64 ] || note "signature test: не получен хеш staging-набора"
printf 'x' > $W/rl/drvget.sig
run "confirm rl ${HASH:0:8}"
grep -q "неверный размер .sig" $T/out || note "malformed publisher signature was not rejected: $(tail -c 300 $T/out)"
[ -z "$(ls -A $T/drv)" ] || note "malformed publisher signature left files in /drv: $(ls -la $T/drv)"
rm -f $W/rl/drvget.sig

# 4. честные отказы
expect_refuse() { # ИМЯ подстрока-причины
    rm -rf $T/sb $T/drv; mkdir -p $T/sb $T/drv
    run "install $U/$1/drvget.rec"
    grep -q "$2" $T/out || note "$1: ожидали отказ '$2': $(tail -c 300 $T/out)"
    grep -q "Установка НЕ выполнена" $T/out && note "$1: показан отчёт об установке, а нужен был отказ"
    [ -z "$(ls $T/drv)" ] || note "$1: записано в /drv"
    [ -z "$(ls $T/sb | grep '^s-')" ] || note "$1: остались подготовленные файлы: $(ls $T/sb)"
}
expect_refuse gpl "GPL"
expect_refuse nolic "лицензия файла не найдена"
expect_refuse bin "бинарник"
expect_refuse pin "SHA-256 файла rl.c не совпал с рецептом"
expect_refuse nohash "не закреплён SHA-256"
expect_refuse winsys "только исходник .c или .h"
expect_refuse nosuch "HTTP 404"
run "install http://127.0.0.1:$PORT/rl/drvget.rec"; grep -q "разрешён только https" $T/out || note "http не отвергнут: $(tail -c 200 $T/out)"
rm -rf $T/sb $T/drv; mkdir -p $T/sb $T/drv
run "install rl"   # имя ищется во встроенном PCI-каталоге; только staging, без установки
 grep -q "SHA-256 рецепта совпал с системным каталогом" $T/out || note "install по имени: рецепт не закреплён каталогом"
[ -z "$(ls $T/drv)" ] || note "install по каталогу записал в /drv без confirm"
# свой каталог рецептов
run "base $U"; grep -q "каталог рецептов теперь $U" $T/out || note "base: $(tail -c 200 $T/out)"
run "install rl"; grep -q "Установка НЕ выполнена" $T/out || note "install по имени через base: $(tail -c 300 $T/out)"
# запрошено имя, а в рецепте другое
mkdir -p $W/wrongname && cp drivers/rl/* $W/wrongname/
run "install wrongname"; grep -q "а рецепт называет драйвер 'rl'" $T/out || note "несовпадение имени не отвергнуто: $(tail -c 200 $T/out)"
# сервер с неизвестной цепочкой: TLS должен отказать
mkdir -p $T/ssl2 && python3 -I - $T/chain $T/ssl2/roots.der <<'PY'
import sys, shutil
shutil.copy(sys.argv[1] + "/expired.der", sys.argv[2])   # корневые - не те, что подписали сервер
PY
rm -f $T/out; timeout 120 $T/kloader -s $T/sb -i kcc/include -y $T/ssl2 -d $T/drv -a "install $U/rl/drvget.rec" $T/drvget.elf >$T/out 2>&1
grep -q "TLS" $T/out && ! grep -q "Установка НЕ выполнена" $T/out || note "TLS с чужими корневыми не отклонён: $(tail -c 300 $T/out)"
rm -f $T/ssl2/roots.der; rm -f $T/out; timeout 120 $T/kloader -s $T/sb -i kcc/include -y $T/ssl2 -d $T/drv -a "install $U/rl/drvget.rec" $T/drvget.elf >$T/out 2>&1
grep -q "нет корневых сертификатов" $T/out || note "без корневых соединение не отклонено: $(tail -c 200 $T/out)"

# 5. drvman в режиме do: те же find/install, две установки подряд в одном процессе (арена kcc переиспользуется), отказы, метка лицензии в .rec, честная ошибка списка PCI
rundm() { rm -f $T/out; timeout 240 $T/kloader -s $T/sb -i kcc/include -y $T/ssl -d $T/drv -a "$1" $T/drvman.elf >$T/out 2>&1; rc=$?; }
rm -rf $T/sb $T/drv; mkdir -p $T/sb $T/drv
rundm "do scan"
grep -q "drvman: автопоиск невозможен: список PCI недоступен" $T/out || note "drvman scan: честный отказ при недоступном PCI API отсутствует: $(tail -c 400 $T/out)"
[ -z "$(ls $T/drv)" ] || note "drvman scan записал пакет без PCI-инвентаризации"
rundm "do match 10ec:8139"
grep -q "drvman: PCI 10ec:8139: найден кандидат rl" $T/out || note "drvman match: $(tail -c 400 $T/out)"
grep -q "SHA-256 рецепта совпал с системным каталогом" $T/out || note "drvman match: рецепт не закреплён каталогом"
[ -z "$(ls $T/drv)" ] || note "drvman match: пакет установлен без явного подтверждения"
rundm "do base $U;pci;find rl;install;pkgs;verify rl"
grep -q "drvman: набор готов к установке" $T/out || note "drvman find: $(tail -c 400 $T/out)"
grep -q "собрано, образ ELF" $T/out && grep -q "drvman: драйвер rl установлен в /drv" $T/out || note "drvman install: $(tail -c 600 $T/out)"
grep -q "drvman: список PCI недоступен" $T/out || note "drvman: нет честного сообщения о недоступном списке PCI: $(grep -a 'drvman:' $T/out | head -5)"
grep -qE "drvman: пакет rl \| rl +v1 +лицензия: BSD-3-Clause +не запущен +сверка: SHA-256 OK, локально" $T/out || note "drvman pkgs (лицензия из .rec): $(grep -a 'пакет' $T/out | head -5)"
grep -qE "drvman: пакет rl \| права запрошены: MMIO DMA PCIDEV NETDEV +подтверждены: MMIO DMA PCIDEV NETDEV" $T/out || note "drvman pkgs: права"
grep -q "drvman: пакет rl | источник: $U/rl/drvget.rec" $T/out || note "drvman pkgs: источник"
grep -q "verify rl: OK" $T/out || note "drvman verify"
chk=$($T/dgcheck $T/drv rl 0x$(python3 -I - <<'PY'
import re
s = open('caps.h', encoding='utf-8').read()
v = {}
for m in re.finditer(r'(CAP_[A-Z_]+)\s*=\s*(1u?\s*<<\s*(\d+)|0x[0-9a-fA-F]+)', s):
    v[m.group(1)] = (1 << int(m.group(3))) if m.group(3) else int(m.group(2), 16)
print('%x' % (v['CAP_MMIO'] | v['CAP_DMA'] | v['CAP_PCIDEV'] | v['CAP_NETDEV']))
PY
) 2>&1)
echo "$chk" | grep -q "^OK rl v1" || note "drvman: пакет в /drv неверен: $chk"
rundm "do find rl;install;find rl;install;remove rl;pkgs"
[ "$(grep -ac 'drvman: драйвер rl установлен в /drv' $T/out)" = 2 ] || note "drvman: две установки подряд: $(grep -a 'drvman:' $T/out | tail -6)"
grep -q "drvman: пакет rl удалён" $T/out && [ ! -e $T/drv/rl.kdrv ] || note "drvman remove"
rundm "do install"; grep -q "нечего устанавливать" $T/out || note "drvman install без find не отклонён"
rm -rf $T/sb $T/drv; mkdir -p $T/sb $T/drv
rundm "do base $U;find gpl;install"; grep -q "drvget: отказ: .*GPL" $T/out && grep -q "drvman: поиск не удался" $T/out && grep -q "нечего устанавливать" $T/out && [ -z "$(ls $T/drv)" ] || note "drvman GPL: $(tail -c 400 $T/out)"
rundm "do find bin;find pin;find nosuch"; [ "$(grep -ac 'drvman: поиск не удался' $T/out)" = 3 ] && [ -z "$(ls $T/drv)" ] || note "drvman отказы: $(tail -c 500 $T/out)"
rundm "do frobnicate"; grep -q "неизвестная команда" $T/out && grep -q "drvman: готово с ошибками" $T/out || note "drvman: неизвестная команда принята: $(tail -c 200 $T/out)"

if [ $fail = 0 ]; then echo "drvget e2e: install -> confirm -> сборка kcc на устройстве -> /drv, отказы GPL/бинарник/без лицензии/неверный хеш/http/.sys/чужая цепочка - всё как задумано"; else echo "drvget e2e: провалов $fail"; exit 1; fi
