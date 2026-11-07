# KDRV publisher trust journal

Publisher packages are verified against the key set derived from the compiled
root keys. The kernel replays the signed update journal before accepting the
first publisher-signed driver:

- `/drv/.rotate` — concatenated fixed-size `kdrv_transition_t` records in
  strictly increasing generation order. The file contains the complete chain
  from a compiled root key to the currently active publishers.
- `/drv/.revoke` — concatenated fixed-size `kdrv_revocation_t` statements,
  strictly increasing by generation. Every statement is replayed; revoked keys
  remain revoked even when later statements add further revocations.
- `/drv/*.sig` — package signatures, checked against the resulting active key
  set and the package bytes.

The transition and revocation statements are Ed25519-authenticated. A malformed
or invalid journal fails closed for publisher-signed packages. Local packages
without a publisher signature retain the existing local-authentication path.

The kernel does not accept a CRC-only trust snapshot as an authority. This
prevents an attacker from replacing the active public-key set merely by editing
a filesystem file and recomputing its CRC. The signed journal itself must remain
available on disk.

**Rollback boundary:** without TPM-backed monotonic storage or another hardware
anti-rollback anchor, an attacker able to restore or remove the entire driver
filesystem can restore an older valid signed journal. The kernel reports no
stronger guarantee than the platform can provide.
