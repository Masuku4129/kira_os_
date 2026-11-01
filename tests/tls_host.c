// Хост-тест клиента TLS 1.3 Киры (userland/tls.c) против настоящего сервера OpenSSL (ci/https_once.py --port):
// рукопожатие X25519 + ChaCha20-Poly1305, проверка Finished сервера, данные больше 16 КБ, close_notify.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "tls.h"

i64 k_net_send(i64 h, const void* p, u64 n) { return send((int)h, p, n, 0); }
i64 k_net_recv(i64 h, void* p, u32 n, u32 ms) {
    struct pollfd f = { (int)h, POLLIN, 0 };
    if (poll(&f, 1, (int)ms) <= 0) return -5;
    return recv((int)h, p, n, 0);
}
i64 k_random(void* b, u64 n) { FILE* f = fopen("/dev/urandom", "rb"); size_t r = fread(b, 1, n, f); fclose(f); return r == n ? 1 : -1; }

static tls_t t;
static char buf[65536];
static u8 roots[8192];

/* tls порт корневой.der ok|untrusted */
int main(int argc, char** argv) {
    int port = argc > 1 ? atoi(argv[1]) : 18443;
    size_t rl = 0;
    if (argc > 2) { FILE* f = fopen(argv[2], "rb"); if (f) { rl = fread(roots, 1, sizeof roots, f); fclose(f); } }
    int want_fail = argc > 3 && !strcmp(argv[3], "untrusted");
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(0x7f000001);
    if (connect(s, (struct sockaddr*)&a, sizeof a)) { printf("TLS TEST FAILED: no server\n"); return 1; }
    int rc = tls_connect(&t, s, "kira.test", 5000, rl ? roots : 0, (u32)rl, (i64)time(0));
    if (want_fail) {
        if (rc == 0 || !t.err || !strstr(t.err, "not trusted")) { printf("TLS TEST FAILED: untrusted server accepted (%s)\n", t.err ? t.err : "-"); return 1; }
        printf("tls: self-signed server rejected: %s - passed\n", t.err);
        return 0;
    }
    if (rc) { printf("TLS TEST FAILED: %s\n", t.err); return 1; }
    if (rl && !t.verified) { printf("TLS TEST FAILED: chain not verified\n"); return 1; }
    const char* req = "GET /tls-test HTTP/1.0\r\nHost: kira.test\r\n\r\n";
    if (tls_send(&t, req, (u32)strlen(req)) != (i64)strlen(req)) { printf("TLS TEST FAILED: send\n"); return 1; }
    size_t total = 0; i64 r;
    while ((r = tls_recv(&t, buf + total, (u32)(sizeof buf - 1 - total), 5000)) > 0) total += (size_t)r;
    buf[total] = 0;
    tls_close(&t); close(s);
    int ok = r == 0 && strstr(buf, "kira-tls-ok TLSv1.3 TLS_CHACHA20_POLY1305_SHA256 GET /tls-test HTTP/1.0") && strstr(buf, "0999-0123456789abcdef") && total > 20000;
    if (!ok) { printf("TLS TEST FAILED: r=%lld total=%zu err=%s\n%.200s\n", r, total, t.err ? t.err : "-", buf); return 1; }
    printf("tls: TLS 1.3 with OpenSSL (X25519, ChaCha20-Poly1305, Finished, CertificateVerify%s), %zu bytes over several records, close_notify passed\n",
           t.verified ? ", X.509 chain to root + name + dates" : "", total);
    return 0;
}
