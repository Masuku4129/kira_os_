#!/usr/bin/env python3
"""Стенд подписанного манифеста приложений (А3, docs/APP_ISOLATION.md): из apps.manifest, который собрала сборка (tools/kfstool appman: SHA-256 всех /bin),
делает набор файлов для диска сценария: верная подпись, подпись вторым ключом (ротация), подменённый манифест. Подписывает tools/kdrvsign.py - тем же
инструментом, которым подписывает издатель. Ключи - тестовые векторы RFC 8032 (закрытые части общеизвестны): их принимает только ядро, собранное с
KIRA_TEST_TRUST=1 (метка kira.testtrust), боевое ядро им не доверяет.

    python3 ci/apps_fixture.py [ВЫХОДНОЙ_КАТАЛОГ]       по умолчанию ci-out/apps; нужны apps.manifest в текущем каталоге и python3-cryptography
Файлы: manifest (верный), sig_a (ключ TEST-ED25519-001), sig_b (ключ TEST-ED25519-002), manifest_bad (один байт изменён; подпись от верного не подходит)."""
import os, shutil, subprocess, sys, time

SEEDS = {"A": "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
         "B": "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb"}
KEY_IDS = {"A": "TEST-ED25519-001", "B": "TEST-ED25519-002"}


def missing():
    if not os.path.exists("apps.manifest"):
        return "нет apps.manifest (его делает build_clang.sh)"
    if not os.path.exists("kira.testtrust"):
        return "ядро собрано не с KIRA_TEST_TRUST=1 (нет kira.testtrust): тестовым ключам оно не доверяет"
    try:
        import cryptography  # noqa: F401
    except ImportError:
        return "нет python3-cryptography (pip install cryptography)"
    return None


def build(out, root="."):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives.serialization import Encoding, NoEncryption, PrivateFormat, PublicFormat
    out = os.path.abspath(out)
    os.makedirs(out, exist_ok=True)
    manifest = os.path.join(out, "apps.manifest")
    shutil.copyfile("apps.manifest", manifest)
    res = {"apps_manifest": manifest}
    now = int(time.time())
    for tag, seed in SEEDS.items():
        k = Ed25519PrivateKey.from_private_bytes(bytes.fromhex(seed))
        pem = os.path.join(out, "key%s.pem" % tag)
        open(pem, "wb").write(k.private_bytes(Encoding.PEM, PrivateFormat.PKCS8, NoEncryption()))
        pub = os.path.join(out, "pub%s.hex" % tag)
        open(pub, "w").write(k.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw).hex() + "\n")
        sig = os.path.join(out, "apps.sig.%s" % tag)
        subprocess.run([sys.executable, "-I", os.path.join(root, "tools/kdrvsign.py"), manifest, "--key", pem, "--public-key", pub,
                        "--trust-header", os.path.join(root, "kdrv_trust.h"), "--key-id", KEY_IDS[tag],
                        "--not-before", str(now - 86400), "--not-after", str(now + 10 * 365 * 86400), "--domain", "apps", "--output", sig],
                       check=True, stdout=subprocess.DEVNULL)
        res["apps_sig_" + tag.lower()] = sig
    bad = bytearray(open(manifest, "rb").read())
    bad[40] ^= 0x01                                   # первый байт SHA-256 первой записи: манифест другой, подпись прежняя
    mb = os.path.join(out, "apps.manifest.bad")
    open(mb, "wb").write(bytes(bad))
    res["apps_manifest_bad"] = mb
    return res


if __name__ == "__main__":
    why = missing()
    if why:
        sys.exit("apps_fixture: " + why)
    for n, p in build(sys.argv[1] if len(sys.argv) > 1 else "ci-out/apps").items():
        print("%-18s %s" % (n, p))
