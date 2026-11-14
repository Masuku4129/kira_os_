#!/usr/bin/env python3
"""Тестовая цепочка сертификатов для проверки X.509 Киры (только для тестов, ключи создаются заново при каждом
запуске и нигде не хранятся): корневой RSA-2048 -> промежуточный ECDSA P-384 (подпись sha256WithRSA) ->
сервер ECDSA P-256 (подпись ecdsa-with-SHA384), имена kira.test, *.kira.test, IP 10.0.2.100 и 127.0.0.1.
Ещё: просроченный сертификат сервера и самоподписанный (недоверенный). Выход: каталог с .der и .pem.
Дополнительно (тот же корневой): ed_* - промежуточный Ed25519 (подпись корневого RSA-PSS sha256/соль 32) и сервер
Ed25519 (подпись Ed25519); pss_* - сервер с ключом id-RSASSA-PSS (подпись корневого RSA-PSS sha384/MGF1 sha384/соль 48);
ocsp_*.der - ответы OCSP для сервера основной цепочки (good, delegated, revoked, unknown, stale, forged, other, tryagain)."""
import datetime, ipaddress, os, subprocess, sys
from cryptography import x509
from cryptography.x509.oid import NameOID, ObjectIdentifier, ExtendedKeyUsageOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa, ec, ed25519, padding
from cryptography.x509 import ocsp

out = sys.argv[1]; os.makedirs(out, exist_ok=True)
now = datetime.datetime.now(datetime.timezone.utc)
def name(cn): return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])
def cert(subj, key, issuer, ikey, h, ca, days=(-1, 30), san=None, pad=None, eku=None, path_length=None, key_usage=None, unknown_critical=False):
    b = (x509.CertificateBuilder().subject_name(name(subj)).issuer_name(name(issuer)).public_key(key.public_key())
         .serial_number(x509.random_serial_number()).not_valid_before(now + datetime.timedelta(days=days[0]))
         .not_valid_after(now + datetime.timedelta(days=days[1]))
         .add_extension(x509.BasicConstraints(ca=ca, path_length=path_length), critical=True))
    if san: b = b.add_extension(x509.SubjectAlternativeName(san), critical=False)
    if eku: b = b.add_extension(x509.ExtendedKeyUsage(eku), critical=False)
    if key_usage: b = b.add_extension(key_usage, critical=True)
    if unknown_critical: b = b.add_extension(x509.UnrecognizedExtension(ObjectIdentifier("1.2.3.4.5.6"), b"\\x05\\x00"), critical=True)
    return b.sign(ikey, h, rsa_padding=pad) if pad else b.sign(ikey, h)
def save(n, c): open(os.path.join(out, n + ".der"), "wb").write(c.public_bytes(serialization.Encoding.DER)); open(os.path.join(out, n + ".pem"), "wb").write(c.public_bytes(serialization.Encoding.PEM))
def savekey(n, k): open(os.path.join(out, n + ".key"), "wb").write(k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption()))

rk = rsa.generate_private_key(public_exponent=65537, key_size=2048)
ik = ec.generate_private_key(ec.SECP384R1())
lk = ec.generate_private_key(ec.SECP256R1())
san = [x509.DNSName("kira.test"), x509.DNSName("*.kira.test"), x509.IPAddress(ipaddress.ip_address("10.0.2.100")), x509.IPAddress(ipaddress.ip_address("127.0.0.1"))]
root = cert("Kira Test Root", rk, "Kira Test Root", rk, hashes.SHA256(), True, (-1, 365),
            key_usage=x509.KeyUsage(digital_signature=False, content_commitment=False, key_encipherment=False,
            data_encipherment=False, key_agreement=False, key_cert_sign=True, crl_sign=True,
            encipher_only=False, decipher_only=False))
inter = cert("Kira Test Intermediate", ik, "Kira Test Root", rk, hashes.SHA256(), True,
             key_usage=x509.KeyUsage(digital_signature=False, content_commitment=False, key_encipherment=False,
             data_encipherment=False, key_agreement=False, key_cert_sign=True, crl_sign=True,
             encipher_only=False, decipher_only=False))
leaf = cert("kira.test", lk, "Kira Test Intermediate", ik, hashes.SHA384(), False, san=san,
            eku=[ExtendedKeyUsageOID.SERVER_AUTH],
            key_usage=x509.KeyUsage(digital_signature=True, content_commitment=False, key_encipherment=False,
            data_encipherment=False, key_agreement=False, key_cert_sign=False, crl_sign=False,
            encipher_only=False, decipher_only=False))
old = cert("kira.test", lk, "Kira Test Intermediate", ik, hashes.SHA384(), False, (-60, -30), san=san)
sk = ec.generate_private_key(ec.SECP256R1())
selfs = cert("kira.test", sk, "kira.test", sk, hashes.SHA256(), False, san=san)
for n, c in [("root", root), ("inter", inter), ("leaf", leaf), ("expired", old), ("self", selfs)]: save(n, c)
# ---- Negative RFC 5280 path/usage fixtures ----
bad_ku = cert("kira.test", lk, "Kira Test Intermediate", ik, hashes.SHA384(), False, san=san,
              key_usage=x509.KeyUsage(digital_signature=False, content_commitment=False, key_encipherment=True,
              data_encipherment=False, key_agreement=False, key_cert_sign=False, crl_sign=False,
              encipher_only=False, decipher_only=False))
bad_eku = cert("kira.test", lk, "Kira Test Intermediate", ik, hashes.SHA384(), False, san=san,
               eku=[ExtendedKeyUsageOID.CLIENT_AUTH])
bad_critical = cert("kira.test", lk, "Kira Test Intermediate", ik, hashes.SHA384(), False, san=san, unknown_critical=True)
bad_inter_ku = cert("Kira Test Intermediate", ik, "Kira Test Root", rk, hashes.SHA256(), True,
                    key_usage=x509.KeyUsage(digital_signature=True, content_commitment=False, key_encipherment=False,
                    data_encipherment=False, key_agreement=False, key_cert_sign=False, crl_sign=False,
                    encipher_only=False, decipher_only=False))
for n, c in [("bad_ku", bad_ku), ("bad_eku", bad_eku), ("bad_critical", bad_critical), ("bad_inter_ku", bad_inter_ku)]:
    save(n, c)

# pathLenConstraint=0 issuer with one subordinate CA below it must reject.
top0 = cert("Kira PathLen Zero", ik, "Kira Test Root", rk, hashes.SHA256(), True,
            path_length=0, key_usage=x509.KeyUsage(digital_signature=False, content_commitment=False,
            key_encipherment=False, data_encipherment=False, key_agreement=False, key_cert_sign=True,
            crl_sign=True, encipher_only=False, decipher_only=False))
subk = ec.generate_private_key(ec.SECP256R1())
subca = cert("Kira Subordinate CA", subk, "Kira PathLen Zero", ik, hashes.SHA256(), True,
             key_usage=x509.KeyUsage(digital_signature=False, content_commitment=False, key_encipherment=False,
             data_encipherment=False, key_agreement=False, key_cert_sign=True, crl_sign=True,
             encipher_only=False, decipher_only=False))
subleaf = cert("kira.test", lk, "Kira Subordinate CA", subk, hashes.SHA256(), False, san=san,
               eku=[ExtendedKeyUsageOID.SERVER_AUTH],
               key_usage=x509.KeyUsage(digital_signature=True, content_commitment=False, key_encipherment=False,
               data_encipherment=False, key_agreement=False, key_cert_sign=False, crl_sign=False,
               encipher_only=False, decipher_only=False))
path0_directleaf = cert("kira.test", lk, "Kira PathLen Zero", ik, hashes.SHA256(), False, san=san,
                       eku=[ExtendedKeyUsageOID.SERVER_AUTH],
                       key_usage=x509.KeyUsage(digital_signature=True, content_commitment=False, key_encipherment=False,
                       data_encipherment=False, key_agreement=False, key_cert_sign=False, crl_sign=False,
                       encipher_only=False, decipher_only=False))
for n, c in [("path0_inter", top0), ("path0_subca", subca), ("path0_leaf", subleaf)]:
    save(n, c)
save("path0_directleaf", path0_directleaf)

savekey("leaf", lk); savekey("self", sk)
open(os.path.join(out, "chain.pem"), "wb").write(leaf.public_bytes(serialization.Encoding.PEM) + inter.public_bytes(serialization.Encoding.PEM))
savekey("root", rk)
PEM = serialization.Encoding.PEM

# ---- Ed25519: промежуточный подписан корневым RSA-PSS, сервер подписан Ed25519 ----
ek = ed25519.Ed25519PrivateKey.generate(); elk = ed25519.Ed25519PrivateKey.generate()
pss256 = padding.PSS(mgf=padding.MGF1(hashes.SHA256()), salt_length=32)
einter = cert("Kira Test Ed Intermediate", ek, "Kira Test Root", rk, hashes.SHA256(), True, pad=pss256)
eleaf = cert("kira.test", elk, "Kira Test Ed Intermediate", ek, None, False, san=san)
save("ed_inter", einter); save("ed_leaf", eleaf); savekey("ed_leaf", elk)
open(os.path.join(out, "ed_chain.pem"), "wb").write(eleaf.public_bytes(PEM) + einter.public_bytes(PEM))

# ---- ключ id-RSASSA-PSS (openssl: python cryptography такие ключи не создаёт), сертификат подписан корневым RSA-PSS sha384 ----
def ossl(*a): subprocess.run(["openssl", *a], check=True, capture_output=True, cwd=out)
ossl("genpkey", "-algorithm", "RSA-PSS", "-pkeyopt", "rsa_keygen_bits:2048", "-pkeyopt", "rsa_pss_keygen_md:sha256",
     "-pkeyopt", "rsa_pss_keygen_mgf1_md:sha256", "-pkeyopt", "rsa_pss_keygen_saltlen:32", "-out", "pss_leaf.key")
ossl("req", "-new", "-key", "pss_leaf.key", "-subj", "/CN=kira.test", "-out", "pss_leaf.csr")
open(os.path.join(out, "pss_ext.cnf"), "w").write("subjectAltName=DNS:kira.test,DNS:*.kira.test,IP:10.0.2.100,IP:127.0.0.1\nbasicConstraints=CA:FALSE\n")
ossl("x509", "-req", "-in", "pss_leaf.csr", "-CA", "root.pem", "-CAkey", "root.key", "-set_serial", "0x5155", "-days", "30", "-sha384",
     "-sigopt", "rsa_padding_mode:pss", "-sigopt", "rsa_pss_saltlen:48", "-sigopt", "rsa_mgf1_md:sha384", "-extfile", "pss_ext.cnf", "-out", "pss_leaf.pem")
ossl("x509", "-in", "pss_leaf.pem", "-outform", "DER", "-out", "pss_leaf.der")
for f in ("pss_leaf.csr", "pss_ext.cnf"): os.unlink(os.path.join(out, f))
open(os.path.join(out, "pss_chain.pem"), "wb").write(open(os.path.join(out, "pss_leaf.pem"), "rb").read())

# ---- ответы OCSP для сервера основной цепочки (издатель - промежуточный P-384) ----
def resp(status, cert_=leaf, key=ik, issuer=inter, rid=inter, certs=None, tu=(-1, 1), alg=hashes.SHA1(), sigh=hashes.SHA256(), rev=None):
    b = ocsp.OCSPResponseBuilder().add_response(cert=cert_, issuer=issuer, algorithm=alg, cert_status=status,
            this_update=now + datetime.timedelta(hours=tu[0] * 1), next_update=now + datetime.timedelta(days=tu[1]),
            revocation_time=rev, revocation_reason=x509.ReasonFlags.key_compromise if rev else None).responder_id(ocsp.OCSPResponderEncoding.HASH, rid)
    if certs: b = b.certificates(certs)
    return b.sign(key, sigh).public_bytes(serialization.Encoding.DER)
def putocsp(n, d): open(os.path.join(out, "ocsp_%s.der" % n), "wb").write(d)
G, R, U = ocsp.OCSPCertStatus.GOOD, ocsp.OCSPCertStatus.REVOKED, ocsp.OCSPCertStatus.UNKNOWN
putocsp("good", resp(G))
dk = ec.generate_private_key(ec.SECP256R1())
dcert = cert("Kira Test OCSP Responder", dk, "Kira Test Intermediate", ik, hashes.SHA256(), False, eku=[x509.oid.ExtendedKeyUsageOID.OCSP_SIGNING])
putocsp("deleg", resp(G, key=dk, rid=dcert, certs=[dcert], alg=hashes.SHA256(), sigh=hashes.SHA384()))      # уполномоченный ответчик, CertID с SHA-256
nk = ec.generate_private_key(ec.SECP256R1())
ncert = cert("Not An OCSP Responder", nk, "Kira Test Intermediate", ik, hashes.SHA256(), False)             # без id-kp-OCSPSigning
putocsp("noeku", resp(G, key=nk, rid=ncert, certs=[ncert]))
putocsp("revoked", resp(R, rev=now - datetime.timedelta(days=1)))
putocsp("unknown", resp(U))
putocsp("stale", resp(G, tu=(-240, -3)))                                                                    # nextUpdate в прошлом
fk = ec.generate_private_key(ec.SECP384R1())
fcert = cert("Kira Test Intermediate", fk, "Kira Test Root", rk, hashes.SHA256(), True)                    # то же имя, чужой ключ
putocsp("forged", resp(G, key=fk, rid=fcert))
putocsp("other", resp(G, cert_=old))                                                                       # про другой сертификат (тот же издатель)
putocsp("tryagain", ocsp.OCSPResponseBuilder.build_unsuccessful(ocsp.OCSPResponseStatus.TRY_LATER).public_bytes(serialization.Encoding.DER))
print("chain:", out)
