// Хост-тест клиента TLS 1.3 Киры (userland/tls.c) против настоящих серверов: Python ssl (ci/https_once.py --port) и
// openssl s_server. Режимы (3-й аргумент):
//   ok        - ChaCha20-Poly1305, цепочка RSA -> P-384 -> P-256, данные больше 16 КБ, close_notify
//   untrusted - самоподписанный сервер: отказ
//   aes       - openssl s_server только с TLS_AES_128_GCM_SHA256
//   ed, pss   - цепочки Ed25519 (схема 0x0807) и RSA-PSS (ключ id-RSASSA-PSS, схемы 0x0809-0x080b)
//   ocsp-none, ocsp-good, ocsp-deleg - openssl s_server со status_file: ответ принят (ocsp=1) или его нет (не ошибка)
//   ocsp-revoked, ocsp-forged, ocsp-other - отказ; ocsp-stale, ocsp-unknown - соединение есть, ocsp=0 с причиной
//   resume    - openssl s_server -naccept 3: полное рукопожатие -> билет -> резюмирование (resumed=1, сервер пишет
//               «Reused») -> испорченный билет (сервер отказывает, полное рукопожатие)
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

static tls_t t, t2;
static char buf[65536];
static u8 roots[8192];
static int port; static size_t rl;

#define FAIL(...) do { printf("TLS TEST FAILED: " __VA_ARGS__); printf("\n"); return 1; } while (0)

static int dial(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(0x7f000001);
    if (connect(s, (struct sockaddr*)&a, sizeof a)) { close(s); return -1; }
    return s;
}
/* Запрос GET и чтение до закрытия; возвращает число байт, буфер завершён нулём. */
static size_t get(tls_t* c, const char* req) {
    if (tls_send(c, req, (u32)strlen(req)) != (i64)strlen(req)) return 0;
    size_t total = 0; i64 r;
    while ((r = tls_recv(c, buf + total, (u32)(sizeof buf - 1 - total), 5000)) > 0) total += (size_t)r;
    buf[total] = 0;
    return total;
}

static int resume_test(void) {
    int s = dial(); if (s < 0) FAIL("no server");
    if (tls_connect(&t, s, "kira.test", 5000, roots, (u32)rl, (i64)time(0), 0)) FAIL("first connection: %s", t.err);
    size_t n = get(&t, "GET / HTTP/1.0\r\n\r\n");
    if (!n || !t.verified || t.resumed || !strstr(buf, "New, TLSv1.3")) FAIL("first connection must be a full handshake (%zu bytes)\n%.200s", n, buf);
    if (!t.ticket.valid || t.ticket.ticket_len < 16) FAIL("server sent no NewSessionTicket");
    tls_ticket_t tk = t.ticket;
    tls_close(&t); close(s);

    s = dial(); if (s < 0) FAIL("no server (2)");
    if (tls_connect(&t2, s, "kira.test", 5000, roots, (u32)rl, (i64)time(0), &tk)) FAIL("resumed connection: %s", t2.err);
    n = get(&t2, "GET / HTTP/1.0\r\n\r\n");
    if (!n || !t2.resumed || !t2.verified || t2.cert_count != 0 || !strstr(buf, "Reused, TLSv1.3")) FAIL("resumption did not happen (resumed=%d cert_count=%u)\n%.300s", t2.resumed, t2.cert_count, buf);
    if (!t2.ticket.valid) FAIL("no new ticket after resumption");
    tls_close(&t2); close(s);

    tk.ticket[tk.ticket_len / 2] ^= 1;                            /* испорченный билет: сервер отказывает, рукопожатие полное */
    s = dial(); if (s < 0) FAIL("no server (3)");
    if (tls_connect(&t, s, "kira.test", 5000, roots, (u32)rl, (i64)time(0), &tk)) FAIL("connection with a bad ticket: %s", t.err);
    n = get(&t, "GET / HTTP/1.0\r\n\r\n");
    if (!n || t.resumed || !t.verified || t.cert_count == 0 || !strstr(buf, "New, TLSv1.3")) FAIL("a bad ticket must fall back to a full handshake\n%.200s", buf);
    tls_close(&t); close(s);
    printf("tls: session resumption with openssl s_server: NewSessionTicket -> PSK (psk_dhe_ke) -> server says Reused, no certificates, identity from the first connection; "
           "a corrupted ticket falls back to a full handshake - passed\n");
    return 0;
}

/* tls порт корневой.der режим */
int main(int argc, char** argv) {
    port = argc > 1 ? atoi(argv[1]) : 18443;
    if (argc > 2) { FILE* f = fopen(argv[2], "rb"); if (f) { rl = fread(roots, 1, sizeof roots, f); fclose(f); } }
    const char* mode = argc > 3 ? argv[3] : "ok";
    if (!strcmp(mode, "resume")) return resume_test();
    int s = dial();
    if (s < 0) FAIL("no server");
    int rc = tls_connect(&t, s, "kira.test", 5000, rl ? roots : 0, (u32)rl, (i64)time(0), 0);
    if (!strcmp(mode, "untrusted")) {
        if (rc == 0 || !t.err || !strstr(t.err, "not trusted")) FAIL("untrusted server accepted (%s)", t.err ? t.err : "-");
        printf("tls: self-signed server rejected: %s - passed\n", t.err);
        return 0;
    }
    if (!strcmp(mode, "ocsp-revoked") || !strcmp(mode, "ocsp-forged") || !strcmp(mode, "ocsp-other")) {
        const char* want = !strcmp(mode, "ocsp-revoked") ? "revoked" : !strcmp(mode, "ocsp-forged") ? "not signed" : "not about";
        if (rc == 0 || !t.err || !strstr(t.err, want)) FAIL("%s: connection must be refused with '%s' (rc=%d err=%s)", mode, want, rc, t.err ? t.err : "-");
        printf("tls: %s stapled OCSP response - connection refused (%s) - passed\n", mode + 5, t.err);
        return 0;
    }
    if (rc) FAIL("%s", t.err);
    if (rl && !t.verified) FAIL("chain not verified");
    const char* req = !strncmp(mode, "ocsp", 4) || !strcmp(mode, "aes") ? "GET / HTTP/1.0\r\n\r\n" : "GET /tls-test HTTP/1.0\r\nHost: kira.test\r\n\r\n";
    size_t total = get(&t, req);
    i64 r = t.state == 3 ? 0 : -1;
    tls_close(&t); close(s);
    if (!strcmp(mode, "aes")) {
        int ok = t.cipher == 0x1301 && strstr(buf, "HTTP/1.0 200 ok") && strstr(buf, "TLS_AES_128_GCM_SHA256") && t.verified;
        if (!ok) FAIL("(AES): cipher %04x r=%lld total=%zu\n%.300s", t.cipher, r, total, buf);
        printf("tls: openssl s_server with only TLS_AES_128_GCM_SHA256 - AES-128-GCM records (AES without tables), chain verified, %zu bytes - passed\n", total);
        return 0;
    }
    if (!strncmp(mode, "ocsp", 4)) {
        int stapled = strcmp(mode, "ocsp-none") != 0;
        int good = !strcmp(mode, "ocsp-good") || !strcmp(mode, "ocsp-deleg");
        if (t.ocsp_stapled != stapled || t.ocsp != good || (!good && stapled && !t.ocsp_why) || (!stapled && t.ocsp_why)) FAIL("%s: stapled=%d ocsp=%d why=%s", mode, t.ocsp_stapled, t.ocsp, t.ocsp_why ? t.ocsp_why : "-");
        if (!strstr(buf, "HTTP/1.0 200 ok")) FAIL("%s: no HTTP answer", mode);
        printf("tls: %s: stapled=%d ocsp=%d%s%s - passed\n", mode, t.ocsp_stapled, t.ocsp, t.ocsp_why ? " - " : "", t.ocsp_why ? t.ocsp_why : "");
        return 0;
    }
    int ok = r == 0 && strstr(buf, "kira-tls-ok TLSv1.3 TLS_CHACHA20_POLY1305_SHA256 GET /tls-test HTTP/1.0") && strstr(buf, "0999-0123456789abcdef") && total > 20000;
    if (!ok) FAIL("r=%lld total=%zu err=%s\n%.200s", r, total, t.err ? t.err : "-", buf);
    if (!strcmp(mode, "ed") && t.cv_scheme != 0x0807) FAIL("ed: CertificateVerify scheme %04x, want 0x0807", t.cv_scheme);
    if (!strcmp(mode, "pss") && (t.cv_scheme < 0x0809 || t.cv_scheme > 0x080b)) FAIL("pss: CertificateVerify scheme %04x, want 0x0809-0x080b", t.cv_scheme);
    printf("tls: TLS 1.3 with %s (X25519, ChaCha20-Poly1305, Finished, CertificateVerify scheme 0x%04x%s), %zu bytes over several records, close_notify passed\n",
           !strcmp(mode, "ed") ? "Ed25519 chain (intermediate signed with RSA-PSS)" : !strcmp(mode, "pss") ? "RSASSA-PSS server key (certificate signed with RSA-PSS sha384)" : "OpenSSL",
           t.cv_scheme, t.verified ? ", X.509 chain to root + name + dates" : "", total);
    return 0;
}
