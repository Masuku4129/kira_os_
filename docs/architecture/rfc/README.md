# KIRA OS — RFC index

**Status:** working drafts, not approved ABI freeze.

| RFC | Contract | Status | Source baseline |
|---|---|---|---|
| [RFC-BOOT-001](RFC-BOOT-001.md) | Unified BootInfo handoff | Draft; current x86 Multiboot2 and ARM64 FDT adapters remain authoritative | main @ bbc7f7ab |
| [RFC-ABI-001](RFC-ABI-001.md) | Syscall and native application ABI | Draft; syscall numbers inventoried, per-ISA register map still requires source trace | main @ bbc7f7ab |
| [RFC-SEC-001](RFC-SEC-001.md) | Capabilities, identity and authority | Draft; static CAP_* model preserved as legacy baseline | main @ bbc7f7ab |
| [RFC-IPC-001](RFC-IPC-001.md) | IPC, shared memory and event semantics | Draft; PORT/SHM/futex facts captured | main @ bbc7f7ab |
| [RFC-TOOL-001](RFC-TOOL-001.md) | KCC compiler context/reentrancy | Draft; global context and self-host failures recorded | main @ bbc7f7ab |
| [RFC-MIND-001](RFC-MIND-001.md) | KIRA Mind telemetry, learning and bounded control | Draft; current safeguards and direct action callbacks inventoried | main @ bbc7f7ab |

## RFC review requirements

Every RFC must be reviewed by the owners of affected subsystems, linked to the contract registry, and include:
- exact source paths/symbols and current behavior;
- alternatives and non-goals;
- security and failure semantics;
- x86-64/ARM64 differences;
- compatibility/migration plan;
- executable positive/negative tests;
- explicit decision status: Draft / Accepted / Rejected / Superseded.

Draft RFCs do not authorize implementation that breaks current ABI or changes main.
