#!/usr/bin/env python3
"""HTTPS-ответ для проверки TLS 1.3 Киры. Без --port - через stdin/stdout (QEMU guestfwd на 10.0.2.100:443);
«--port N» - один раз на TCP 127.0.0.1:N (хост-тест). «--chain каталог» - цепочка из ci/mkchain.py (сервер +
промежуточный), «--self каталог» - самоподписанный оттуда же; без них - самоподписанный, созданный при запуске.
«--prefix ed_|pss_» - с «--chain»: цепочка Ed25519 (ed_chain.pem) или с ключом RSA-PSS (pss_chain.pem).
Тело - больше 16 КБ (несколько записей TLS); первая строка называет версию и шифр, которые выбрал сервер.
«--serve каталог» - отдавать файлы каталога (GET /путь), для drvget; «--loop» с «--port» - обслуживать соединения без конца.
"""
import os, ssl, socket, subprocess, sys, tempfile, threading, time

args = sys.argv[1:]
opt = lambda k: args[args.index(k) + 1] if k in args else None

def bridge(port):
    """stdin/stdout <-> TCP 127.0.0.1:port (сервер openssl s_server, нужный там, где Python ssl не умеет: OCSP stapling, билеты)."""
    s = socket.create_connection(("127.0.0.1", port))
    def up():
        while True:
            b = sys.stdin.buffer.read1(16384)
            if not b: break
            s.sendall(b)
        try: s.shutdown(socket.SHUT_WR)
        except OSError: pass
    threading.Thread(target=up, daemon=True).start()
    while True:
        b = s.recv(16384)
        if not b: break
        sys.stdout.buffer.write(b); sys.stdout.buffer.flush()

if opt("--bridge"):                       # постоянный сервер хозяина (сценарий запускает его сам): билеты живут между соединениями
    bridge(int(opt("--bridge"))); sys.exit(0)
if opt("--ocsp"):                         # openssl s_server с прикреплённым ответом OCSP из ci/mkchain.py, на одно соединение
    d = opt("--chain")
    probe = socket.socket(); probe.bind(("127.0.0.1", 0)); port = probe.getsockname()[1]; probe.close()
    srv = subprocess.Popen(["openssl", "s_server", "-quiet", "-accept", str(port), "-cert", d + "/leaf.pem", "-key", d + "/leaf.key",
                            "-cert_chain", d + "/inter.pem", "-tls1_3", "-www", "-status_file", d + "/ocsp_" + opt("--ocsp") + ".der"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(100):
        try: socket.create_connection(("127.0.0.1", port), timeout=1).close(); break
        except OSError: time.sleep(0.1)
    try: bridge(port)
    finally: srv.terminate()
    sys.exit(0)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version = ssl.TLSVersion.TLSv1_3
if opt("--chain"):
    pre = opt("--prefix") or ""
    ctx.load_cert_chain(os.path.join(opt("--chain"), pre + "chain.pem"), os.path.join(opt("--chain"), pre + "leaf.key"))
elif opt("--self"):
    ctx.load_cert_chain(os.path.join(opt("--self"), "self.pem"), os.path.join(opt("--self"), "self.key"))
else:
    d = tempfile.mkdtemp()
    subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes",
                    "-keyout", d + "/k.pem", "-out", d + "/c.pem", "-days", "1", "-subj", "/CN=kira.test"],
                   check=True, capture_output=True)
    ctx.load_cert_chain(d + "/c.pem", d + "/k.pem")


def serve(rd, wr):
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
    if opt("--serve"):          # файловый сервер для drvget: GET /путь -> файл из каталога; HTTP/1.0 с Content-Length, потом закрытие
        parts = line.split(" ")
        rel = os.path.normpath(parts[1].lstrip("/")) if len(parts) > 1 else ""
        fp = os.path.join(opt("--serve"), rel)
        if rel.startswith("..") or not os.path.isfile(fp):
            body, hd = b"not found\n", b"HTTP/1.0 404 Not Found\r\n"
        else:
            body, hd = open(fp, "rb").read(), b"HTTP/1.0 200 OK\r\n"
        call(s.write, hd + b"Content-Length: %d\r\n\r\n" % len(body) + body)
    else:
        body = ("kira-tls-ok %s %s %s\n" % (s.version(), s.cipher()[0], line)).encode() + b"".join(b"%04d-0123456789abcdef\n" % i for i in range(1000))
        call(s.write, b"HTTP/1.0 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
    try:
        s.unwrap()
    except ssl.SSLError:
        pass
    pump()

if opt("--port"):
    ls = socket.socket(); ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", int(opt("--port")))); ls.listen(8)
    print("ready", flush=True)
    while True:                  # --loop: соединения до тех пор, пока процесс не убьют (хост-тест drvget); иначе одно
        conn, _ = ls.accept()
        try:
            serve(lambda: conn.recv(16384), conn.sendall)
        except (EOFError, ssl.SSLError, OSError):
            pass
        conn.close()
        if "--loop" not in args: break
else:
    def wr(b): sys.stdout.buffer.write(b); sys.stdout.buffer.flush()
    serve(lambda: sys.stdin.buffer.read1(16384), wr)
