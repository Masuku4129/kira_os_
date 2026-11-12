# KIRA OS — Linux application compatibility architecture

**Статус:** целевое направление; dedicated Linux application compatibility runtime в main baseline пока не подтверждён.  
**Связанные документы:** target architecture, master plan, contract registry, compatibility test matrix.

## 1. Цель и границы

Запускать Linux user-space applications непосредственно на KIRA OS без установки Linux kernel или полной Linux guest OS. Полная Linux в VM остаётся отдельной возможностью KIRA Hypervisor.

Не входит в эту спецификацию:
- запуск Linux kernel modules;
- обещание полной совместимости со всеми дистрибутивами/приложениями;
- подмена KIRA kernel Linux kernel;
- прямой доступ Linux-приложений к MMIO, DMA или host devices.

## 2. Архитектурная цепочка

```text
ELF64 / ELF interpreter
        ↓
Linux process personality + loader
        ↓
Linux syscall ABI dispatcher
        ↓
Linux semantic adapters (signals, futex, mmap, epoll, procfs...)
        ↓
KIRA compatibility host interface (capability mediated)
        ↓
KIRA process/memory/IPC/VFS/network/graphics/audio services
        ↓
KIRA kernel and HAL
```

Compatibility runtime выполняется в user space там, где это возможно. Syscall entry/dispatch может использовать узкий kernel trap boundary, но Linux semantics и API emulation не должны превращаться в произвольный privileged subsystem.

## 3. Loader requirements

- ELF32/ELF64 parsing only for explicitly supported class, endianness, machine and ABI.
- Validate headers, program headers, alignment, overflow, file bounds, segment overlap, permissions and entry point.
- PT_INTERP and dynamic linker loading; DT_NEEDED/SONAME resolution with deterministic library search policy.
- Relocations, TLS, auxiliary vector, initial stack, environment and argv.
- Reject unsupported ABI flags and malformed objects fail-closed.
- Distinguish Linux ELF from KIRA-native ELF by explicit ABI/personality metadata or launch policy; never infer compatibility solely from ELF magic.

## 4. Linux syscall and userspace semantics

Maintain a versioned syscall coverage table by ISA and ABI profile. Candidate families:
- process/thread: clone/clone3 subset, execve, exit, wait, getpid/tid, robust teardown;
- memory: mmap/munmap/mprotect/brk, shared mappings, futex;
- signals: signal masks, delivery, alternate stack and restart semantics;
- files: openat family, read/write, stat, directory iteration, poll/select/epoll;
- IPC: pipes, eventfd, timerfd, Unix sockets, shared memory as required;
- networking: socket, bind, connect, accept, send/recv, options;
- time: clocks, nanosleep, timers;
- identity and limits: uid/gid model, rlimits, resource accounting;
- procfs/sysfs/devices: virtualized, filtered views, never expose host internals by default.

Linux syscall numbers and structure layouts differ by ISA; x86-64 and AArch64 tables must be independently sourced and tested. POSIX conformance is not equivalent to Linux ABI compatibility.

## 5. Libraries and graphics

- Define supported libc/dynamic-linker profiles (e.g. glibc and musl separately); do not claim one implies the other.
- Resolve shared libraries from trusted, versioned compatibility roots.
- Define graphics/audio/input adapters independently (initially a constrained software-rendered/display and audio path is acceptable if real end-to-end).
- Avoid silently substituting APIs with materially different semantics.

## 6. Security model

- Each Linux app runs in a KIRA process/container with explicit capabilities and resource quotas.
- Filesystem root, network, devices, IPC and graphics access are mediated by KIRA host services.
- No direct host kernel memory, device register or arbitrary host filesystem access.
- Syscall translation validates all pointers, lengths, flags, descriptors and nested structures.
- Namespaces, credentials and privilege semantics must be mapped explicitly; unsupported privilege escalation operations fail closed.
- Audit app identity, binary provenance, library resolution and denied operations.

## 7. Delivery phases

1. ABI profile and syscall inventory for x86-64 and AArch64.
2. ELF loader and deterministic dynamic-linker/library model.
3. Minimal process, memory, file and clock syscalls.
4. Threading, futex, signals and synchronization.
5. sockets, epoll/poll and networking.
6. procfs/sysfs compatibility views and common libraries.
7. graphics/audio/input and application packaging.
8. real application matrix, performance and security hardening.

## 8. First vertical slice

Select one small, redistributable Linux user-space application with a known architecture and dependency closure. Demonstrate:
- verified ELF load and dynamic linking;
- process/thread creation and exit;
- file read/write within a sandbox;
- one IPC or socket operation;
- capability denial for an ungranted operation;
- malformed ELF and invalid syscall rejection;
- correct teardown without leaks.

A successful loader-only demo is not application compatibility.

## 9. Acceptance evidence

For each supported application record exact binary hash, version, ISA, ABI profile, library closure, syscall coverage, test environment, expected/actual behavior, known limitations, performance and security tests. QEMU, Limbo and physical hardware are separate evidence classes.
