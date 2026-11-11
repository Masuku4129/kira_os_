#include <stdio.h>
#include <string.h>
#include "kdrv_trust.h"
#include "pk.h"

int main(void) {
    uint8_t fingerprint[PK_SHA256];
    pk_hash(PK_SHA256, kdrv_trust_keys[0].public_key, sizeof kdrv_trust_keys[0].public_key, fingerprint);
    if (memcmp(fingerprint, kdrv_trust_keys[0].key_id, KDRV_MANIFEST_KEY_ID_SIZE) != 0) {
        puts("kdrv_trust: FAIL public-key fingerprint does not match key ID");
        return 1;
    }
    if (kdrv_trust_keys[0].revoked != 0) {
        puts("kdrv_trust: FAIL initial owner key unexpectedly revoked");
        return 1;
    }
    puts("kdrv_trust: configured public-key fingerprint matches its key ID");
    return 0;
}
