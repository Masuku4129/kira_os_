#!/usr/bin/env python3
"""Подписанный манифест загрузки KIRA.SIG (формат - bootsig.h, описание - docs/SECURE_BOOT.md).
Подписание делает openssl (Ed25519, ключ остаётся у выпускающего и в образ не попадает).

  kirasig.py keygen PRIV.pem PUB.bin            новая пара ключей выпуска: закрытый PEM (права 0600) и открытый ключ, 32 байта
  kirasig.py pubkey PRIV.pem PUB.bin            открытый ключ (32 байта) из закрытого
  kirasig.py sign --key PRIV.pem --version N --out KIRA.SIG KIRA.ELF [INITRD.IMG]
  kirasig.py verify --pub PUB.bin [--min-version N] KIRA.SIG KIRA.ELF [INITRD.IMG]   проверка тем же форматом на хосте
  kirasig.py cheader PUB.bin OUT.h              заголовок C с открытым ключом для сборки загрузчика (KIRA_SECURE=1)"""
import argparse, hashlib, os, struct, subprocess, sys, tempfile

MAGIC = b"KIRASIG1"
DOMAIN = b"KIRA-BOOT-MANIFEST-V1\0"
HEADER, ENTRY, SIGLEN, MAX_ENTRIES = 32, 56, 64, 4
NAMES = ["KIRA.ELF", "INITRD.IMG"]


def openssl(*args, data=None):
    r = subprocess.run(["openssl", *args], input=data, capture_output=True)
    if r.returncode != 0:
        sys.exit("kirasig: openssl %s: %s" % (args[0], r.stderr.decode(errors="replace").strip()))
    return r.stdout


def pub_from_priv(priv):
    der = openssl("pkey", "-in", priv, "-pubout", "-outform", "DER")
    if len(der) != 44:
        sys.exit("kirasig: %s - не ключ Ed25519" % priv)
    return der[-32:]


def body(version, files):
    ents = b""
    for name, path in files:
        data = open(path, "rb").read()
        ents += name.encode().ljust(16, b"\0") + struct.pack("<Q", len(data)) + hashlib.sha256(data).digest()
    return MAGIC + struct.pack("<IIII", version, 0, len(files), 0) + b"\0" * 8 + ents


def cmd_keygen(a):
    fd = os.open(a.priv, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    os.close(fd)
    out = openssl("genpkey", "-algorithm", "ed25519")
    with open(a.priv, "wb") as f:
        f.write(out)
    open(a.pub, "wb").write(pub_from_priv(a.priv))
    print("kirasig: ключ выпуска создан: %s (хранить вне образа и вне git), открытый %s" % (a.priv, a.pub))


def cmd_pubkey(a):
    open(a.pub, "wb").write(pub_from_priv(a.priv))


def cmd_sign(a):
    if len(a.files) < 1 or len(a.files) > 2:
        sys.exit("kirasig: нужно ядро и, если есть, initrd")
    if not 0 <= a.version <= 0xFFFFFFFF:
        sys.exit("kirasig: номер выпуска вне диапазона u32")
    b = body(a.version, list(zip(NAMES, a.files)))
    with tempfile.TemporaryDirectory() as td:
        m = os.path.join(td, "msg")
        open(m, "wb").write(DOMAIN + b)
        sig = openssl("pkeyutl", "-sign", "-inkey", a.key, "-rawin", "-in", m)
    if len(sig) != SIGLEN:
        sys.exit("kirasig: подпись не 64 байта")
    open(a.out, "wb").write(b + sig)
    print("kirasig: %s: выпуск %d, файлов %d, %d байт" % (a.out, a.version, len(a.files), len(b + sig)))


def cmd_verify(a):
    blob = open(a.sig, "rb").read()
    pub = open(a.pub, "rb").read()
    if len(blob) < HEADER + ENTRY + SIGLEN or blob[:8] != MAGIC:
        sys.exit("kirasig: формат манифеста неверен")
    n = struct.unpack_from("<I", blob, 16)[0]
    if not 1 <= n <= MAX_ENTRIES or len(blob) != HEADER + ENTRY * n + SIGLEN:
        sys.exit("kirasig: формат манифеста неверен")
    version = struct.unpack_from("<I", blob, 8)[0]
    b, sig = blob[:-SIGLEN], blob[-SIGLEN:]
    der = bytes.fromhex("302a300506032b6570032100") + pub
    with tempfile.TemporaryDirectory() as td:
        for nm, data in (("msg", DOMAIN + b), ("sig", sig), ("pub", der)):
            open(os.path.join(td, nm), "wb").write(data)
        r = subprocess.run(["openssl", "pkeyutl", "-verify", "-pubin", "-keyform", "DER", "-inkey", os.path.join(td, "pub"),
                            "-rawin", "-in", os.path.join(td, "msg"), "-sigfile", os.path.join(td, "sig")], capture_output=True)
    if r.returncode != 0:
        sys.exit("kirasig: подпись манифеста неверна")
    if version < a.min_version:
        sys.exit("kirasig: выпуск %d ниже порога %d" % (version, a.min_version))
    for (name, path), i in zip(zip(NAMES, a.files), range(n)):
        e = blob[HEADER + ENTRY * i: HEADER + ENTRY * (i + 1)]
        data = open(path, "rb").read()
        if e[:16].rstrip(b"\0").decode() != name or struct.unpack_from("<Q", e, 16)[0] != len(data) or e[24:56] != hashlib.sha256(data).digest():
            sys.exit("kirasig: %s не совпадает с манифестом" % name)
    if n != len(a.files):
        sys.exit("kirasig: в манифесте %d файлов, дано %d" % (n, len(a.files)))
    print("kirasig: манифест верен, выпуск %d" % version)


def cmd_cheader(a):
    pub = open(a.pub, "rb").read()
    if len(pub) != 32:
        sys.exit("kirasig: открытый ключ должен быть 32 байта")
    rows = ",\n    ".join(", ".join("0x%02x" % x for x in pub[i:i + 8]) for i in range(0, 32, 8))
    open(a.out, "w").write("/* Создан tools/kirasig.py: открытый ключ выпуска Ed25519 для проверки KIRA.SIG (секретов нет). */\n"
                           "static const unsigned char kira_boot_pubkey[32] = {\n    %s\n};\n" % rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="c", required=True)
    p = sp.add_parser("keygen"); p.add_argument("priv"); p.add_argument("pub"); p.set_defaults(f=cmd_keygen)
    p = sp.add_parser("pubkey"); p.add_argument("priv"); p.add_argument("pub"); p.set_defaults(f=cmd_pubkey)
    p = sp.add_parser("sign"); p.add_argument("--key", required=True); p.add_argument("--version", type=int, required=True)
    p.add_argument("--out", required=True); p.add_argument("files", nargs="+"); p.set_defaults(f=cmd_sign)
    p = sp.add_parser("verify"); p.add_argument("--pub", required=True); p.add_argument("--min-version", type=int, default=0)
    p.add_argument("sig"); p.add_argument("files", nargs="+"); p.set_defaults(f=cmd_verify)
    p = sp.add_parser("cheader"); p.add_argument("pub"); p.add_argument("out"); p.set_defaults(f=cmd_cheader)
    a = ap.parse_args()
    a.f(a)


main()
