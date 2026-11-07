#ifndef KIRA_KDRV_TRUST_H
#define KIRA_KDRV_TRUST_H
/* KIRA OS owner trust anchor.
 * Public key only. The matching private key MUST remain offline and outside Git/ISO.
 * Ed25519 public key, raw 32-byte encoding.
 * SHA-256 fingerprint:
 * aa85df60c87d3666ecdcc95e80b707e087cb54d1efce548124016eee93402c5f
 */
#include <stdint.h>
#define KDRV_TRUST_KEY_COUNT 1u
#define KDRV_TRUST_KEY_ID "aa85df60c87d3666"
static const uint8_t kdrv_trust_ed25519_keys[KDRV_TRUST_KEY_COUNT][32] = {
    {
        0x86,0xef,0x20,0x0e,0x9a,0x9f,0xab,0xb3,
        0x58,0x1d,0xea,0xc8,0xa3,0x1e,0xac,0x91,
        0x21,0x9c,0x44,0xb8,0x4b,0xb4,0x4c,0xb8,
        0x05,0x83,0x0b,0x47,0xcf,0xf5,0x54,0xe8
    }
};
#endif
