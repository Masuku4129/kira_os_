# RFC-SEC-001 — capabilities, identity and authority model

**Статус:** DRAFT — preserve current behavior while designing object capabilities.  
**База:** main @ bbc7f7ab948e72511aed113b69043add10ec8350.

## 1. Current implementation facts

- `caps.h` defines bitmask capabilities for graphics, network, UDP, audio, IPC, threads, files, grants, system files, driver MMIO/IRQ/DMA/PCI/netdev, PCI listing and JIT.
- `caps.c:caps_for_program(name)` uses a static trusted-program-name table; unknown names receive zero rights. Names prefixed `local-` explicitly receive no capabilities.
- `proc.c:spawn()` assigns capabilities at process creation. Driver processes receive a separate manager-provided capability mask and binding slot.
- `syscall.c:cap_needed()` centrally checks many syscall capabilities. File operations perform operation-specific checks in fsrv. Driver calls use per-device binding state.
- Current process capabilities are a bitmask; no general user-visible object-capability transfer/revoke API was identified in the inspected main paths.

## 2. Target model

A capability is an unforgeable reference to a specific object or service endpoint plus a rights mask. Global identity/policy determines whether a subject may receive it; possession alone is not enough if the object has been revoked or the subject is terminated.

## 3. Required semantics

- subject identity: process, service, driver, runtime instance, VM manager and Mind controller;
- object identity: file, directory, port, shared-memory object, device, socket, service, VM and resource;
- rights: operation-specific and non-ambient;
- grant: issuer, recipient, object, rights, constraints, expiry/lease and audit reason;
- delegation: only explicit delegable rights; cannot amplify authority;
- revocation: object generation or revocation table, deterministic failure after revoke;
- inheritance: deny by default; explicit manifest/launch policy only;
- transfer: IPC capability passing with atomic ownership/reference semantics;
- audit: grant, deny, delegate, revoke and use of privileged operations.

## 4. Migration from bitmask model

1. Keep existing CAP_* bits and checks unchanged as legacy process-level policy.
2. Inventory every check and map it to object-level rights.
3. Add typed kernel handles for object capabilities with owner and generation.
4. Introduce grant/delegate/revoke APIs behind privileged manager interfaces.
5. Migrate IPC endpoints and device handles first, then files/network/graphics.
6. Retain compatibility shim until all native apps and runtimes have migrated.
7. Test revocation races, stale handles, cross-process confusion and confused-deputy paths.

## 5. Required invariants

- fail closed for unknown capability, stale generation, revoked object and malformed handle;
- no ambient global names as sole authority;
- rights are monotonic non-amplifying under delegation;
- process exit revokes owned non-persistent capabilities and releases references;
- device capabilities are scoped to exact device instance and lifecycle generation;
- Mind cannot grant itself capabilities or bypass resource owner policy;
- guest VM capabilities never map directly to host physical access.

## 6. Tests

- unknown process name has zero rights;
- capability denial is logged and does not mutate state;
- cross-process handle reuse and stale generation;
- grant/revoke concurrent with IPC and device removal;
- delegation cannot add rights;
- process/service crash releases references;
- driver slot reuse does not transfer stale device authority;
- Mind action request without owner approval is denied.
