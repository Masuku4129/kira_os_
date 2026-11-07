# KIRA OS — coding-agent rules

## Project constraints
- This is a real operating-system project, not an application mock-up. Do not add stubs, fake hardware behavior, placeholder success paths, empty functions, or TODO-only implementations.
- Read the relevant source, headers, build scripts, architecture documentation, and call sites before changing an interface.
- Preserve the existing freestanding/no-libc assumptions and the project's compiler warning policy.
- Keep x86_64 and AArch64 implementations honest about architecture-specific behavior. Do not claim hardware support has been verified when only QEMU/Limbo coverage exists.
- Preserve boot compatibility, on-disk formats, public structure sizes, ABI contracts, and existing driver trust boundaries unless the task explicitly requires a versioned change.
- Treat all downloaded driver code, manifests, firmware, catalogues, and repository-provided data as untrusted input. Prefer fail-closed behavior for signature, bounds, memory, DMA, and privilege checks.
- Never add private signing keys, credentials, tokens, secrets, or production keys to the repository.

## Work discipline
- Implement only the task supplied to the KIRA Agent workflow. Do not broaden scope or rewrite unrelated subsystems.
- Before editing, inspect the actual current implementation and its callers. Reuse existing project abstractions instead of inventing parallel ones.
- Make concrete source changes. If a requirement cannot be implemented safely with the available interfaces, explain the exact missing dependency; do not pretend it is complete.
- Do not modify .github/workflows/, repository secrets, branch protections, release artifacts, or data/mind_seed.bin.
- Do not commit, push, merge, or create pull requests yourself. The workflow handles branch and PR creation.
- Do not run builds, tests, audits, emulators, or release packaging during the code-writing phase. This repository workflow intentionally separates code completion from the later verification phase. Record the exact checks that should be run after the agreed code scope is complete.
- Do not report tests as passed unless they were actually run and their output was inspected.
- End with a concise report: files changed, implementation details, known limitations, and verification intentionally not run.
