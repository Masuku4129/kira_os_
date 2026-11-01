#!/usr/bin/env python3
"""HTTPS-ответ для проверки TLS 1.3 Киры. Без аргументов - через stdin/stdout (QEMU guestfwd на 10.0.2.100:443);
«--port N» - один раз на TCP 127.0.0.1:N (хост-тест). Сертификат - самоподписанный ECDSA P-256, создаётся при запуске.
Тело - больше 16 КБ (несколько записей TLS); первая строка называет версию и шифр, которые выбрал сервер."""
import os, ssl, socket, subprocess, sys, tempfile

d = tempfile.mkdtemp()
subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
                "-keyout", d + "/k.pem", "-out", d + "/c.pem", "-days", "1", "-subj", "/CN=kira.test"],
               check=True, capture_output=True)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version = ssl.TLSVersion.TLSv1_3
ctx.load_cert_chain(d + "/c.pem", d + "/k.pem")

if len(sys.argv) > 2 and sys.argv[1] == "--port":
    ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", int(sys.argv[2]))); ls.listen(1)
    print("ready", flush=True)
    conn, _ = ls.accept()
    rd = lambda: conn.recv(16384)
    wr = conn.sendall
else:
    rd = lambda: sys.stdin.buffer.read1(16384)
    def wr(b): sys.stdout.buffer.write(b); sys.stdout.buffer.flush()

inc, out = ssl.MemoryBIO(), ssl.MemoryBIO()
s = ctx.wrap_bio(inc, out, server_side=True)
def pump():
    b = out.read()
    if b: wr(b)
def feed():
    b = rd()
    if not b: raise EOFError
    inc.write(b)
def call(f, *a):
    while True:
        try:
            r = f(*a); pump(); return r
        except ssl.SSLWantReadError:
            pump(); feed()

call(s.do_handshake)
req = b""
while b"\r\n\r\n" not in req:
    req += call(s.read, 4096)
line = req.split(b"\r\n", 1)[0].decode(errors="replace")
body = ("kira-tls-ok %s %s %s\n" % (s.version(), s.cipher()[0], line)).encode() + b"".join(b"%04d-0123456789abcdef\n" % i for i in range(1000))
call(s.write, b"HTTP/1.0 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
try:
    s.unwrap()
except ssl.SSLError:
    pass
pump()
