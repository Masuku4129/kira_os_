#!/usr/bin/env python3
"""Тестовая цепочка сертификатов для проверки X.509 Киры (только для тестов, ключи создаются заново при каждом
запуске и нигде не хранятся): корневой RSA-2048 -> промежуточный ECDSA P-384 (подпись sha256WithRSA) ->
сервер ECDSA P-256 (подпись ecdsa-with-SHA384), имена kira.test, *.kira.test, IP 10.0.2.100 и 127.0.0.1.
Ещё: просроченный сертификат сервера и самоподписанный (недоверенный). Выход: каталог с .der и .pem."""
import datetime, ipaddress, os, sys
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa, ec

out = sys.argv[1]; os.makedirs(out, exist_ok=True)
now = datetime.datetime.now(datetime.timezone.utc)
def name(cn): return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])
def cert(subj, key, issuer, ikey, h, ca, days=(-1, 30), san=None):
    b = (x509.CertificateBuilder().subject_name(name(subj)).issuer_name(name(issuer)).public_key(key.public_key())
         .serial_number(x509.random_serial_number()).not_valid_before(now + datetime.timedelta(days=days[0]))
         .not_valid_after(now + datetime.timedelta(days=days[1]))
         .add_extension(x509.BasicConstraints(ca=ca, path_length=None), critical=True))
    if san: b = b.add_extension(x509.SubjectAlternativeName(san), critical=False)
    return b.sign(ikey, h)
def save(n, c): open(os.path.join(out, n + ".der"), "wb").write(c.public_bytes(serialization.Encoding.DER)); open(os.path.join(out, n + ".pem"), "wb").write(c.public_bytes(serialization.Encoding.PEM))
def savekey(n, k): open(os.path.join(out, n + ".key"), "wb").write(k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))

rk = rsa.generate_private_key(public_exponent=65537, key_size=2048)
ik = ec.generate_private_key(ec.SECP384R1())
lk = ec.generate_private_key(ec.SECP256R1())
san = [x509.DNSName("kira.test"), x509.DNSName("*.kira.test"), x509.IPAddress(ipaddress.ip_address("10.0.2.100")), x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]
root = cert("Kira Test Root", rk, "Kira Test Root", rk, hashes.SHA256(), True, (-1, 365))
inter = cert("Kira Test Intermediate", ik, "Kira Test Root", rk, hashes.SHA256(), True)
leaf = cert("kira.test", lk, "Kira Test Intermediate", ik, hashes.SHA384(), False, san=san)
old = cert("kira.test", lk, "Kira Test Intermediate", ik, hashes.SHA384(), False, (-60, -30), san=san)
sk = ec.generate_private_key(ec.SECP256R1())
selfs = cert("kira.test", sk, "kira.test", sk, hashes.SHA256(), False, san=san)
for n, c in [("root", root), ("inter", inter), ("leaf", leaf), ("expired", old), ("self", selfs)]: save(n, c)
savekey("leaf", lk); savekey("self", sk)
open(os.path.join(out, "chain.pem"), "wb").write(leaf.public_bytes(serialization.Encoding.PEM) + inter.public_bytes(serialization.Encoding.PEM))
print("chain:", out)
