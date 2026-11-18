#!/bin/bash
# Подписанный манифест приложений (А3): манифест собирает tools/kfstool appman, подписывает tools/kdrvsign.py (домен apps, два ключа: действующий и новый
# после ротации), проверяет tests/appman_test.c кодом ядра (appman.c, kdrv_state.c, kdrv_sig.c). Ключи одноразовые, в git не попадают.
set -e
cd "$(dirname "$0")/.."
CC=${CC:-gcc}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
python3 - "$T" <<'PY'
import sys, pathlib
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, NoEncryption, PrivateFormat, PublicFormat
t = pathlib.Path(sys.argv[1])
hdr = ""
for tag, seed in (("A", "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"), ("B", "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb")):
    k = Ed25519PrivateKey.from_private_bytes(bytes.fromhex(seed))
    pub = k.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
    (t / ("key%s.pem" % tag)).write_bytes(k.private_bytes(Encoding.PEM, PrivateFormat.PKCS8, NoEncryption()))
    (t / ("pub%s.hex" % tag)).write_text(pub.hex() + "\n")
    (t / ("pub%s.bin" % tag)).write_bytes(pub)
    hdr += '#define KDRV_TRUST_KEY_ID%s "TEST-APPS-KEY-%s0"\nstatic const unsigned char pub%s[] = {%s};\n' % ("" if tag == "A" else "_NEXT", tag, tag, ",".join("0x%02x" % x for x in pub))
(t / "trust.h").write_text(hdr)
PY
mkdir -p $T/elf
for n in alpha beta gamma; do head -c 3000 /dev/urandom > $T/elf/$n.elf; done
$CC -O1 -Wall -DKFS_NO_THREADS -I. -Iarch/x86_64 -o $T/kfstool tools/kfstool.c kfs.c zstd.c store.c sha256.c sysseal.c cbor.c cas.c
$T/kfstool appman 7 $T/apps.manifest $T/elf/alpha.elf $T/elf/beta.elf $T/elf/gamma.elf
sign() {   # ключ, область, выход
    python3 tools/kdrvsign.py $T/apps.manifest --key $T/key$1.pem --public-key $T/pub$1.hex --trust-header $T/trust.h --key-id "TEST-APPS-KEY-${1}0" \
        --not-before 1700000000 --not-after 4102444800 --domain $2 --output $3 >/dev/null
}
sign A apps $T/apps.sigA; sign B apps $T/apps.sigB; sign A driver $T/apps.sigD
# A3_SRC_DIR - каталог с копиями appman.c, kdrv_sig.c, kdrv_state.c (мутации: tests/a3_mutation_test.sh подставляет испорченные копии)
SRC=${A3_SRC_DIR:-.}
$CC -O1 -g -Wall -Werror -I$SRC -I. -fsanitize=address,undefined -o $T/appman tests/appman_test.c $SRC/appman.c $SRC/kdrv_state.c $SRC/kdrv_sig.c kdrv_fmt.c pk.c crypto.c sha256.c
ASAN_OPTIONS=detect_leaks=0 $T/appman $T/apps.manifest $T/apps.sigA $T/apps.sigB $T/apps.sigD $T/pubA.bin $T/pubB.bin $T/elf
