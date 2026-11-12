# KIRA OS — Windows/Linux application compatibility test matrix

**Purpose:** measurable compatibility evidence; this document is not a claim of current compatibility.

## 1. Required dimensions

Every test row must identify:
- OS family: Linux or Windows;
- application and exact version;
- binary SHA-256 and source/provenance/license;
- target ISA: x86-64 or AArch64;
- ABI profile (Linux syscall/libc profile or Windows API profile);
- runtime/loader build SHA;
- dependency closure and hashes;
- test environment: host, QEMU, Limbo or physical hardware;
- capabilities and quotas granted;
- expected result, actual result, logs and known limitations.

## 2. Test categories

| Category | Linux cases | Windows cases | Required negative cases |
|---|---|---|---|
| Binary loader | ELF class/machine/interpreter/relocations/TLS | PE32+/machine/sections/imports/relocations/TLS | truncation, overflow, invalid RVA/offset, overlap, unsupported ISA |
| Dynamic libraries | musl/glibc profiles, SONAME, symbol versions | DLL imports, ordinals, forwarded exports, API sets | missing dependency, search-path hijack, wrong ABI |
| Process/thread | clone/exec/wait/exit | CreateProcess/CreateThread/ExitProcess | invalid attributes, resource exhaustion, teardown race |
| Memory | mmap/mprotect/brk/shared mapping | VirtualAlloc/Protect/Free/section mapping | W^X violation, out-of-range, invalid handle |
| Synchronization | futex, robust mutex, signals | events, mutexes, semaphores, waits | deadlock, timeout, invalid object, priority abuse |
| Filesystem | openat/stat/dirent/procfs view | CreateFile, file metadata, registry abstraction | traversal, symlink escape, access denied |
| Networking | sockets, DNS, poll/epoll | Winsock subset | ungranted network, malformed buffers, descriptor misuse |
| Graphics/input | selected display/input APIs | selected window/GDI/DirectX profile | invalid surface, cross-process surface access |
| Audio | selected audio API | selected audio API | device unavailable, buffer abuse |
| Isolation | process/container capabilities | process/container capabilities | privilege escalation, sandbox escape attempts |
| Reliability | crash, cancellation, restart | crash, cancellation, restart | host service crash, runtime crash, forced termination |
| Performance | startup, syscall overhead, throughput | startup, API overhead, throughput | pathological syscall/API flood |

## 3. Compatibility levels

- **L0 — Parses:** format parser accepts a binary. Not compatibility.
- **L1 — Loads:** image maps and entry point is reached. Not application compatibility.
- **L2 — Runs core path:** app completes a defined useful scenario using a documented API subset.
- **L3 — Profile compatible:** declared ABI/API profile passes its conformance suite and dependency policy.
- **L4 — Application certified:** exact app/version/ISA passes functional, isolation, reliability and performance acceptance.

Never report an entire OS family as supported from a single application at L2/L4.

## 4. Release gates

No compatibility runtime is production-ready until:
1. parser/verifier fuzzing and malformed-input tests pass;
2. API/syscall coverage is documented and versioned;
3. sandbox and capability negative tests pass;
4. application scenario is repeatable from a clean image;
5. runtime and library provenance are recorded;
6. crash/teardown/resource exhaustion are tested;
7. exact hardware/emulator evidence is attached;
8. limitations are visible to users and package tooling.
