# KIRA OS — next implementation task

Complete the KDRV publisher-trust chain by addressing the explicit security gap recorded in draft PR #42: the system driver catalogue itself is not digitally signed.

Inspect the current catalogue parser, catalogue storage/distribution path, existing Ed25519 verifier, trust-key representation, KDRV installer, kernel driver-load verification, and relevant documentation before editing.

Implement a real, versioned, canonical signed-catalogue format and verification path so catalogue entries are not trusted until the catalogue signature is verified against an explicitly provisioned production trust anchor. Reject malformed, truncated, noncanonical, unknown-key, revoked-key, expired, or missing signatures wherever the catalogue policy requires authentication. Bind the signature to the exact catalogue bytes and format/version; avoid ambiguous text normalization. Keep private signing keys out of Git and do not treat CI/test keys as production trust anchors.

Integrate the verification into every path that consumes the system catalogue, including installer preparation and confirmation; do not leave an unsigned fallback that silently authorizes publisher-required entries. Preserve compatibility only where it is explicitly safe and cannot downgrade a strict entry or installed publisher-authenticated driver.

If the current repository lacks a secure production trust-anchor provisioning or rotation mechanism, do not invent a fake one or commit a private key. Implement the complete safe verification/interface portion that the existing trust model supports, and document the exact remaining provisioning blocker.

Do not run tests, builds, audits, emulators, QEMU, or ISO packaging during this code-writing phase. Do not modify GitHub workflows, signing keys, release artifacts, or data/mind_seed.bin. Make source-code changes only and report every remaining blocker.
