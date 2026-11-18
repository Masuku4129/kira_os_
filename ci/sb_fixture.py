#!/usr/bin/env python3
"""Стенд доверенной загрузки (А2, docs/SECURE_BOOT.md): на лету создаёт тестовые ключи, ключи прошивки (PK, KEK, db в VARS для OVMF
с Secure Boot) и набор образов ESP: верный и испорченные. Всё живёт в ci-out/secureboot (каталог не коммитится); ключи НЕ секретны по
назначению (одноразовые, только для тестов) и нигде кроме ci-out не лежат.

    python3 ci/sb_fixture.py [ВЫХОДНОЙ_КАТАЛОГ] [ядро]       по умолчанию ci-out/secureboot и kira.bin

Нужны: openssl, sbsigntool (sbsign), mtools, gnu-efi (сборка загрузчика), virt-fw-vars (pip install virt-firmware), OVMF.
Образы (имя -> что проверяет):
  good             подписанный загрузчик и верный манифест: грузится, ядро сообщает о доверенной загрузке
  tamper-loader    подписанный загрузчик с одним изменённым байтом: прошивка отвергает, ядро не стартует
  unsigned-loader  загрузчик без подписи db: прошивка отвергает
  tamper-kernel    загрузчик подписан верно, ядро изменено на 1 байт: загрузчик отказывается и пишет причину
  no-manifest      всё подписано, но KIRA.SIG удалён: отказ
  rollback         манифест выпуска 3 при нижнем пороге загрузчика 5: откат отклонён
  release5         тот же загрузчик (порог 0) и подписанный манифест выпуска 5; good - выпуск 3 (А3: счётчик выпуска в TPM, сценарий secureboot-rollback-tpm)
Ключи прошивки: vars-secboot.fd (PK/KEK/db заведены, Secure Boot включён), vars-plain.fd (ключей нет, Secure Boot выключен)."""
import hashlib, os, shutil, struct, subprocess, sys

CODE = "/usr/share/OVMF/OVMF_CODE_4M.secboot.fd"
VARS_TEMPLATE = "/usr/share/OVMF/OVMF_VARS_4M.fd"
OWNER = "4b495241-5345-4355-5245-424f4f542100"                      # произвольный GUID владельца ключей тестового стенда


def sh(*args, **kw):
    r = subprocess.run(args, capture_output=True, **kw)
    if r.returncode != 0:
        raise RuntimeError("%s: %s" % (" ".join(args[:3]), (r.stderr or r.stdout).decode(errors="replace").strip()[-600:]))
    return r.stdout


def missing():
    """Чего не хватает хозяину (None - всё есть)."""
    for tool in ("openssl", "sbsign", "sbverify", "mcopy", "virt-fw-vars", "qemu-system-x86_64"):
        if not shutil.which(tool):
            return "нет " + tool
    for f in (CODE, VARS_TEMPLATE, "/usr/include/efi/efi.h"):
        if not os.path.exists(f):
            return "нет " + f
    return None


def sha(b):
    return hashlib.sha256(b).digest()


def pcr_extend(values):
    """Ожидаемое значение PCR (банк SHA-256, старт с нулей) после внесения данных по порядку: H(PCR || H(данные))."""
    p = b"\0" * 32
    for v in values:
        p = sha(p + sha(v))
    return p.hex()


def first_section(efi):
    """Смещение и размер сырых данных первого раздела PE (там код): сюда безопасно вносить изменение, которое попадает под подпись."""
    pe = struct.unpack_from("<I", efi, 0x3C)[0]
    nsec = struct.unpack_from("<H", efi, pe + 6)[0]
    opt = struct.unpack_from("<H", efi, pe + 20)[0]
    sec = pe + 24 + opt
    for i in range(nsec):
        name, vsize, va, rawsize, rawptr = struct.unpack_from("<8sIIII", efi, sec + 40 * i)
        if rawsize and name.startswith(b".text"):
            return rawptr, rawsize
    raise RuntimeError("в загрузчике нет раздела .text")


def build(out, kernel="kira.bin", root="."):
    out = os.path.abspath(out)
    kernel = os.path.abspath(kernel)
    root = os.path.abspath(root)
    os.makedirs(out, exist_ok=True)
    kdata = open(kernel, "rb").read()
    stamp_src = subprocess.run(["bash", os.path.join(root, "tools/uefi_srchash.sh")], capture_output=True, text=True).stdout.strip()
    stamp = hashlib.sha256(kdata + stamp_src.encode() + open(__file__, "rb").read()).hexdigest()
    res = {"code": CODE, "vars_secboot": os.path.join(out, "vars-secboot.fd"), "vars_plain": os.path.join(out, "vars-plain.fd"),
           "images": {}, "names": {}}
    for n in ("good", "release5", "tamper-loader", "unsigned-loader", "tamper-kernel", "no-manifest", "rollback"):
        res["images"][n] = os.path.join(out, "esp-%s.img" % n)
    keys = os.path.join(out, "keys")
    # Номер выпуска и ожидаемый PCR 9 (манифест, затем ядро) для сценария измеряемой загрузки
    stamp_file = os.path.join(out, "stamp")
    sig_good = os.path.join(out, "KIRA.SIG")
    if os.path.exists(stamp_file) and open(stamp_file).read() == stamp and all(os.path.exists(p) for p in res["images"].values()):
        res["names"]["pcr9"] = pcr_extend([open(sig_good, "rb").read(), kdata])
        return res
    shutil.rmtree(keys, ignore_errors=True)
    os.makedirs(keys)
    kp = lambda n: os.path.join(keys, n)
    tool = lambda n: os.path.join(root, n)

    # ключи: выпуска (Ed25519, их проверяет загрузчик) и прошивки (RSA-2048: PK, KEK, db)
    sh(sys.executable, tool("tools/kirasig.py"), "keygen", kp("rel.pem"), kp("rel.pub"))
    for n in ("pk", "kek", "db"):
        sh("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", kp(n + ".key"), "-out", kp(n + ".crt"),
           "-subj", "/CN=Kira test %s/" % n, "-days", "3650", "-sha256")
    # переменные прошивки: с ключами и Secure Boot - и пустые (Secure Boot выключен)
    sh("virt-fw-vars", "--input", VARS_TEMPLATE, "--output", res["vars_secboot"], "--set-pk", OWNER, kp("pk.crt"),
       "--add-kek", OWNER, kp("kek.crt"), "--add-db", OWNER, kp("db.crt"), "--sb")
    shutil.copyfile(VARS_TEMPLATE, res["vars_plain"])

    # загрузчики: доверенный (ключ выпуска вшит), с повышенным порогом версии и обычный (разработки)
    def loader(name, **env):
        d = os.path.join(out, "ldr-" + name)
        shutil.rmtree(d, ignore_errors=True)
        e = dict(os.environ, KIRA_SECURE="1", KIRA_BOOT_PUBKEY=kp("rel.pub"), **env)
        subprocess.run(["bash", tool("build_uefi.sh"), d], check=True, env=e, cwd=root, capture_output=True)
        return os.path.join(d, "EFI/BOOT/BOOTX64.EFI")
    ldr = loader("sec")
    ldr_min = loader("min5", KIRA_MIN_IMAGE_VERSION="5")
    signed = os.path.join(out, "BOOTX64.signed.efi")
    signed_min = os.path.join(out, "BOOTX64.min5.signed.efi")
    for src, dst in ((ldr, signed), (ldr_min, signed_min)):
        sh("sbsign", "--key", kp("db.key"), "--cert", kp("db.crt"), "--output", dst, src)
        sh("sbverify", "--cert", kp("db.crt"), dst)

    # good: полным путём выпуска - тем же скриптом, которым выпускается настоящий образ
    sh("bash", tool("tools/sbsign_image.sh"), "--loader", ldr, "--kernel", kernel, "--release-key", kp("rel.pem"), "--version", "3",
       "--db-key", kp("db.key"), "--db-cert", kp("db.crt"), "--out", res["images"]["good"])
    sh(sys.executable, tool("tools/kirasig.py"), "sign", "--key", kp("rel.pem"), "--version", "3", "--out", sig_good, kernel)
    # release5: тот же загрузчик и то же ядро, манифест выпуска 5 (подпись верна): после него выпуск 3 с верной подписью обязан быть отвергнут счётчиком TPM
    sh("bash", tool("tools/sbsign_image.sh"), "--loader", ldr, "--kernel", kernel, "--release-key", kp("rel.pem"), "--version", "5",
       "--db-key", kp("db.key"), "--db-cert", kp("db.crt"), "--out", res["images"]["release5"])

    def esp(name, efi, kern=kernel, sig=sig_good):
        args = [sys.executable, tool("tools/mkuefi.py")] + (["--sig", sig] if sig else []) + [efi, kern, res["images"][name], "64"]
        sh(*args)

    # tamper-loader: один байт кода подписанного загрузчика изменён - подпись db больше не сходится
    efi = bytearray(open(signed, "rb").read())
    off, size = first_section(efi)
    efi[off + size // 2] ^= 0x01
    bad = os.path.join(out, "BOOTX64.tampered.efi")
    open(bad, "wb").write(bytes(efi))
    esp("tamper-loader", bad)
    # unsigned-loader: тот же загрузчик без подписи db
    esp("unsigned-loader", ldr)
    # tamper-kernel: загрузчик и манифест верны, ядро - с одним изменённым байтом (манифест остаётся от настоящего ядра)
    k = bytearray(kdata)
    k[len(k) // 2] ^= 0x01
    bad_k = os.path.join(out, "KIRA.tampered.elf")
    open(bad_k, "wb").write(bytes(k))
    esp("tamper-kernel", signed, kern=bad_k)
    # no-manifest: манифеста нет совсем
    esp("no-manifest", signed, sig=None)
    # rollback: манифест выпуска 3, загрузчик требует не ниже 5
    esp("rollback", signed_min)

    open(stamp_file, "w").write(stamp)
    res["names"]["pcr9"] = pcr_extend([open(sig_good, "rb").read(), kdata])
    return res


if __name__ == "__main__":
    why = missing()
    if why:
        sys.exit("sb_fixture: " + why)
    r = build(sys.argv[1] if len(sys.argv) > 1 else "ci-out/secureboot", sys.argv[2] if len(sys.argv) > 2 else "kira.bin")
    for n, p in r["images"].items():
        print("%-16s %s" % (n, p))
    print("ожидаемый PCR 9:", r["names"]["pcr9"])
