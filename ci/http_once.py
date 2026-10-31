#!/usr/bin/env python3
"""HTTP-ответ для проверки TCP Киры: QEMU (guestfwd) соединяет этот процесс с подключением гостя к 10.0.2.100:80.
Тело больше трёх сегментов TCP - проверяется приём по порядку и закрытие соединения сервером."""
import sys
data = b""
while b"\r\n\r\n" not in data:
    chunk = sys.stdin.buffer.read1(4096)
    if not chunk:
        break
    data += chunk
line = data.split(b"\r\n", 1)[0].decode(errors="replace")
body = b"kira-tcp-ok " + line.encode() + b"\n" + b"".join(b"%04d-0123456789abcdef\n" % i for i in range(150))
sys.stdout.buffer.write(b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n" % len(body) + body)
sys.stdout.buffer.flush()
