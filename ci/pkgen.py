#!/usr/bin/env python3
"""Векторы для tests/pk_test.c от python cryptography (OpenSSL): подписи Ed25519 и RSA-PSS с разными хэшами,
хэшем MGF1 и длиной соли, верные и испорченные (ожидание 1/0). Ключи создаются заново при каждом запуске.
Использование: pkgen.py файл [число-ключей-Ed25519]"""
import hashlib, os, random, sys
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ed25519, padding, rsa

L = 2**252 + 27742317777372353535851937790883648493
out = open(sys.argv[1], "w")
n_ed = int(sys.argv[2]) if len(sys.argv) > 2 else 60
rnd = random.Random(os.urandom(8))
h = lambda b: b.hex() if b else "-"
for i in range(n_ed):
    k = ed25519.Ed25519PrivateKey.generate()
    pub = k.public_key().public_bytes_raw()
    m = os.urandom(rnd.choice([0, 1, 5, 64, 127, 128, 129, 300]))
    sig = k.sign(m)
    out.write("ed %s %s %s 1\n" % (pub.hex(), h(m), sig.hex()))
    bad = bytearray(sig); bad[rnd.randrange(64)] ^= 1 << rnd.randrange(8)
    out.write("ed %s %s %s 0\n" % (pub.hex(), h(m), bytes(bad).hex()))
    out.write("ed %s %s %s 0\n" % (pub.hex(), h(m + b"x"), sig.hex()))
    s = int.from_bytes(sig[32:], "little")
    if s + L < 2**256:                       # S + L: то же значение по модулю L, но S >= L - обязан быть отказ
        out.write("ed %s %s %s 0\n" % (pub.hex(), h(m), (sig[:32] + (s + L).to_bytes(32, "little")).hex()))
H = {32: hashes.SHA256, 48: hashes.SHA384, 64: hashes.SHA512}
NAME = {32: "sha256", 48: "sha384", 64: "sha512"}
for bits in (2048, 3072):
    key = rsa.generate_private_key(public_exponent=65537, key_size=bits)
    nb = key.public_key().public_numbers()
    n = nb.n.to_bytes(bits // 8, "big"); e = nb.e.to_bytes(3, "big")
    for alg in H:
        for mgf in H:
            for salt in (0, 20, alg, 32, 64):
                m = os.urandom(50)
                hh = hashlib.new(NAME[alg], m).digest()
                sig = key.sign(m, padding.PSS(mgf=padding.MGF1(H[mgf]()), salt_length=salt), H[alg]())
                fmt = lambda a, mg, sl, sg, ok: out.write("pss %s %s %d %d %d %s %s %d\n" % (n.hex(), e.hex(), a, mg, sl, hh.hex(), sg.hex(), ok))
                fmt(alg, mgf, salt, sig, 1)
                bad = bytearray(sig); bad[rnd.randrange(len(sig))] ^= 1 << rnd.randrange(8)
                fmt(alg, mgf, salt, bytes(bad), 0)
                fmt(alg, mgf, salt + 1, sig, 0)                                  # другая длина соли
                fmt(alg, [x for x in H if x != mgf][0], salt, sig, 0)            # другой хэш MGF1
out.close()
