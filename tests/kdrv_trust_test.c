#include <stdio.h>
#include <string.h>
#include "kdrv_trust.h"
#include "pk.h"

static int hex_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int main(void) {
    uint8_t source_key[32], fingerprint[PK_SHA256];
    FILE *f = fopen("keys/KIRA_OWNER_ED25519_PUBLIC.hex", "rb");
    if (!f) { puts("kdrv_trust: FAIL owner public-key file is missing"); return 1; }
    unsigned digits = 0;
    int high = -1, ch, bad = 0;
    while ((ch = fgetc(f)) != EOF) {
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
        int v = hex_value(ch);
        if (v < 0 || digits >= 64) { bad = 1; break; }
        if ((digits & 1u) == 0) high = v;
        else source_key[digits / 2] = (uint8_t)((high << 4) | v);
        digits++;
    }
    if (ferror(f)) bad = 1;
    fclose(f);
    if (bad || digits != 64 || memcmp(source_key, kdrv_trust_keys[0].public_key, sizeof source_key) != 0) {
        puts("kdrv_trust: FAIL compiled key differs from keys/KIRA_OWNER_ED25519_PUBLIC.hex"); return 1;
    }
    pk_hash(PK_SHA256, source_key, sizeof source_key, fingerprint);
    if (memcmp(fingerprint, kdrv_trust_keys[0].key_id, KDRV_MANIFEST_KEY_ID_SIZE) != 0) {
        puts("kdrv_trust: FAIL public-key fingerprint does not match key ID"); return 1;
    }
    if (kdrv_trust_keys[0].revoked != 0) {
        puts("kdrv_trust: FAIL initial owner key unexpectedly revoked"); return 1;
    }
    puts("kdrv_trust: compiled key matches owner public-key file and SHA-256 key ID");
    return 0;
}