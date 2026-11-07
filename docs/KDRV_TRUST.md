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

The trust snapshot is loaded once and cached for the current boot. Install the
complete updated journals before rebooting; the kernel does not hot-reload
publisher trust while drivers are running.

The transition and revocation statements are Ed25519-authenticated. At boot,
the kernel replays historical statements by signature and generation rather
than rejecting a previously accepted transition merely because its issuance
window has since expired. New package signatures still use current-time checks.
A malformed or invalid journal fails closed for publisher-signed packages. Local packages
without a publisher signature retain the existing local-authentication path.

The kernel does not accept a CRC-only trust snapshot as an authority. This
prevents an attacker from replacing the active public-key set merely by editing
a filesystem file and recomputing its CRC. The signed journal itself must remain
available on disk.

**Rollback boundary:** without TPM-backed monotonic storage or another hardware
anti-rollback anchor, an attacker able to restore or remove the entire driver
filesystem can restore an older valid signed journal. The kernel reports no
stronger guarantee than the platform can provide.

## Offline journal authoring

Private publisher keys remain outside the repository and the OS image.

- `tools/kdrvrotate.py` creates a signed transition record. Supply the current
  publisher's PEM private key and matching raw public key, old/new 16-character
  key IDs, the new raw public key, a generation, and a validity interval.
- `tools/kdrvrevoke.py` creates a signed revocation statement. Supply an
  active issuer key and one or more 16-character IDs to revoke.
- Use `--append` to atomically append a record to an existing local journal.
  The tools enforce each journal's fixed record size, maximum of 32 records,
  and increasing generation within that file.
- Generations are global across both journals. Choose each new generation
  greater than the last generation in **both** `.rotate` and `.revoke`.
  The kernel merges the two journals by generation and rejects duplicates or
  non-increasing order.
- Copy the resulting binary files to `/drv/.rotate` and `/drv/.revoke`.
  Keep the complete journals; they are replayed from the compiled root key at
  boot. Do not replace them with only the newest record.

The tools verify that each PEM key matches the supplied public key. The kernel
still verifies the issuer against its live trust state; a record signed by an
unknown or revoked key is rejected.

To sign a package with the compiled root key, pass `--trust-header` to
`tools/kdrvsign.py`. To sign with a key introduced by a verified rotation,
omit `--trust-header` and pass that key's matching `--public-key`,
`--key-id`, and offline PEM private key. The kernel, not the host signing
utility, is authoritative about whether the rotated key is currently trusted
or revoked.
