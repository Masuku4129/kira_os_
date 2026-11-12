# ADR-0002: KDRV anti-rollback trust anchor

- Status: Accepted — strong anti-rollback unavailable until a hardware-backed provider exists
- Date: 2026-11-12
- Scope: KDRV signed catalogue and package lifecycle

## Context

A signed catalogue proves publisher authenticity and integrity, but it does not
by itself prevent an attacker from restoring an older disk image together with
an older catalogue-version file. CRCs, alternating sectors, generations and
ordinary writable KiraFS files improve corruption recovery; none is a
non-rollbackable trust anchor.

The current repository inventory contains no TPM 2.0, secure-element,
firmware monotonic-counter, authenticated UEFI-variable or equivalent
platform service. The KDRV trust module exposes callbacks for a version floor,
but a callback alone cannot prove that its storage survives whole-disk rollback.

## Decision

1. Strong anti-rollback is **not available** in the current release baseline.
2. The KDRV trust API requires the platform backend to explicitly assert
   `KDRV_TRUST_STORAGE_HW_MONOTONIC`. The bit may only be set after the
   platform has validated a TPM NV index, secure element, authenticated
   firmware variable with rollback protection, or equivalent non-rollbackable
   storage. A regular file, CRC, dual-slot record, or disk generation is not
   sufficient.
3. Missing or unknown storage capability flags fail closed with
   `KDRV_TRUST_STORAGE_ERROR`; a catalogue cannot be accepted or advance its
   floor without the hardware-backed contract.
4. Host tests may emulate the callback contract to exercise policy logic, but
   this is not evidence that the platform has a real monotonic anchor.
5. Until a provider is implemented and tested, the UI and release notes must
   not claim secure anti-rollback. Publisher signature verification and
   ordinary version comparison remain distinct controls.
6. Recovery must be designed with the hardware anchor: key revocation,
   interrupted updates, device replacement, recovery authorization and
   counter exhaustion must not permit silent downgrade.

## Consequences

- Current KDRV catalogue verification is intentionally unavailable to a
  production caller that cannot provide a validated hardware-backed anchor.
- The next implementation task is platform discovery and provisioning, not
  writing a version number to KiraFS and calling it secure.
- QEMU-only emulation can validate protocol/state transitions, not physical
  TPM/secure-element persistence or resistance to firmware/disk rollback.
