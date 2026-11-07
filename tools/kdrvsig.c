/* kdrvsig: wraps a raw Ed25519 signature in the KDRV detached-signature envelope.
   Usage: kdrvsig package.kdrv signature.raw output.sig [trusted-key-id] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kdrv_format.h"
#include "kdrv_trust.h"

static unsigned char* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    if (fseek(f, 0, SEEK_END) != 0) exit(1);
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) exit(1);
    unsigned char* b = (unsigned char*)malloc((size_t)n + 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "kdrvsig: read failed: %s\n", path);
        exit(1);
    }
    fclose(f);
    *size = (size_t)n;
    return b;
}

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: kdrvsig package.kdrv signature.raw output.sig [key-id]\n");
        return 2;
    }
    const char* key_id = argc == 5 ? argv[4] : KDRV_TRUST_KEY_ID;
    if (strlen(key_id) != 16) {
        fprintf(stderr, "kdrvsig: key id must contain exactly 16 hex characters\n");
        return 2;
    }
    for (unsigned i = 0; i < 16; i++) {
        char ch = key_id[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            fprintf(stderr, "kdrvsig: key id must be lowercase hexadecimal\n");
            return 2;
        }
    }
    size_t pkg_len, raw_len;
    unsigned char* pkg = read_file(argv[1], &pkg_len);
    unsigned char* raw = read_file(argv[2], &raw_len);
    if (pkg_len == 0 || pkg_len > UINT32_MAX) {
        fprintf(stderr, "kdrvsig: package size is outside the supported range\\n");
        return 1;
    }
    if (raw_len != 64) {
        fprintf(stderr, "kdrvsig: Ed25519 signature must be exactly 64 bytes\n");
        return 1;
    }
    kdrv_sig_t sig;
    memset(&sig, 0, sizeof sig);
    sig.magic = KDRV_SIG_MAGIC;
    sig.format = KDRV_SIG_FORMAT;
    sig.sig_size = sizeof sig.signature;
    memcpy(sig.key_id, key_id, sizeof sig.key_id);
    memcpy(sig.signature, raw, sizeof sig.signature);
    if (kdrv_sig_check(pkg, (uint32_t)pkg_len, &sig, sizeof sig) != KDRV_OK) {
        fprintf(stderr, "kdrvsig: signature does not verify against the configured KIRA trust key\n");
        return 1;
    }
    FILE* out = fopen(argv[3], "wb");
    if (!out || fwrite(&sig, 1, sizeof sig, out) != sizeof sig) {
        perror(argv[3]);
        return 1;
    }
    fclose(out);
    return 0;
}
