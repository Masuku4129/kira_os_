# ADR-0002: x86-64 TLB shootdown must fail closed

- **Status:** Proposed (implementation under exact-head CI in PR #155)
- **Date:** 2026-11-14
- **Owners:** KIRA OS kernel architecture

## Context

An address-space unmap can be followed by release or reuse of its physical backing pages. Other CPUs may still hold translations for the old mapping. The previous x86-64 shootdown waited 200 ms, logged a warning, cleared the global request lock and returned even when a target CPU had not acknowledged the invalidation. A late IPI could then observe a later request's shared payload or clear the wrong request's pending state.

This creates a memory-safety failure: stale translations may remain usable after the physical page has been reassigned.

## Decision

Use a serialized, generation-bound shootdown protocol:

1. One sender owns the global shootdown request at a time.
2. The sender writes the immutable `root/va/pages` payload and publishes a nonzero 64-bit generation before sending IPIs.
3. Every target CPU invalidates the requested translation and then stores the exact generation in its per-CPU acknowledgement slot.
4. The sender accepts only acknowledgements equal to the current generation.
5. The sender releases the request lock and returns only after all CPUs in the target snapshot acknowledge.
6. The 200 ms threshold emits a diagnostic warning only. It never releases the lock, changes the request payload, or permits backing-memory reclamation.

## Consequences

- **Safety:** A delayed acknowledgement from an older request cannot satisfy a newer request. Pages cannot be reclaimed while a target CPU may still use an old translation.
- **Liveness:** A permanently unresponsive CPU can block shootdown completion and therefore block dependent teardown/reclamation. This is intentional fail-closed behavior.
- **Recovery:** A future CPU-offline protocol must prove the failed CPU is halted and cannot resume before removing it from a shootdown target set. A software `failed` flag alone is not proof.
- **Scope:** This ADR covers x86-64 software-IPI shootdown. ARM64 broadcast TLBI and ASID lifecycle require a separate audit and decision.

## Rejected alternatives

- **Return after timeout:** rejected because it permits stale translations and use-after-free.
- **Clear the pending bit on timeout and continue:** rejected because a late IPI can acknowledge a newer request or use overwritten request data.
- **Silently ignore the unresponsive CPU:** rejected because marking a CPU failed does not prove it stopped executing or cannot resume.

## Verification requirements

- Host tests prove exact-generation matching, partial acknowledgement rejection, stale-generation rejection and empty-target completion.
- Exact-head x86-64 build and host suite pass.
- QEMU SMP, multi-CPU boot/soak and memory/process lifecycle scenarios pass.
- Fault injection for delayed/missing IPI acknowledgement is still required before this ADR can be accepted.
