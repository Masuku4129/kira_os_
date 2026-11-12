# KIRA OS owner trust key

The public key below is the owner-controlled Ed25519 trust anchor generated for KIRA OS.

- Raw public key: `KIRA_OWNER_ED25519_PUBLIC.hex` (32 bytes, hexadecimal).
- SHA-256 fingerprint: `aa85df60c87d3666ecdcc95e80b707e087cb54d1efce548124016eee93402c5f`.
- The matching private key is held offline by the project owner. It must never be committed, embedded in an ISO, or placed in CI secrets without an explicit release-signing design.

This file records the public trust material only. It does not by itself enable KDRV signature enforcement. Kernel-side verification, signed manifest format, key rotation/revocation, persistent anti-rollback, and installer integration must be implemented and tested before driver packages can be described as authenticated.
