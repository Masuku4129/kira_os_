/* pem2der вход.pem выход.der - сертификаты из PEM (связка корневых системы: ca-certificates.crt, cert.pem)
   подряд в DER: хранилище корневых сертификатов Киры (/etc/ssl/roots.der). Печатает их число. */
#include <stdio.h>
#include <string.h>

static int b64(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "использование: pem2der вход.pem выход.der\n"); return 2; }
    FILE* in = fopen(argv[1], "r");
    if (!in) { perror(argv[1]); return 1; }
    FILE* out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }
    static char line[4096];
    static unsigned char der[65536];
    int inside = 0, count = 0; unsigned n = 0, acc = 0, bits = 0;
    while (fgets(line, sizeof line, in)) {
        if (!strncmp(line, "-----BEGIN CERTIFICATE-----", 27)) { inside = 1; n = acc = bits = 0; continue; }
        if (!strncmp(line, "-----END CERTIFICATE-----", 25)) {
            if (inside && n) { fwrite(der, 1, n, out); count++; }
            inside = 0; continue;
        }
        if (!inside) continue;
        for (char* p = line; *p; p++) {
            int v = b64((unsigned char)*p);
            if (v < 0) continue;
            acc = acc << 6 | (unsigned)v; bits += 6;
            if (bits >= 8) { bits -= 8; if (n < sizeof der) der[n++] = (unsigned char)(acc >> bits); acc &= (1u << bits) - 1; }
        }
    }
    fclose(in); fclose(out);
    printf("%d\n", count);
    return count ? 0 : 1;
}
