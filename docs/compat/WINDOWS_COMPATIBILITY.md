# KIRA OS — Windows application compatibility architecture

**Статус:** целевое направление; dedicated Windows PE/Win32/NT user-mode compatibility runtime в main baseline пока не подтверждён.  
**Связанные документы:** target architecture, master plan, contract registry, compatibility test matrix.

## 1. Цель и границы

Запускать поддерживаемые Windows user-mode applications непосредственно на KIRA OS через PE loader и совместимые Win32/необходимые NT user-mode semantics — без установки Windows. Полная Windows в VM остаётся отдельной возможностью KIRA Hypervisor.

Не входит в scope:
- загрузка Windows kernel;
- выполнение Windows kernel drivers;
- обещание совместимости с любым Windows приложением;
- прямой доступ приложения к KIRA kernel internals или аппаратуре.

## 2. Архитектурная цепочка

```text
PE/COFF image + import tables
        ↓
Windows process loader + DLL loader
        ↓
Win32 API / selected NT user-mode semantics
        ↓
Windows compatibility subsystem
        ↓
KIRA compatibility host interface (capability mediated)
        ↓
KIRA process/memory/IPC/VFS/network/graphics/audio services
        ↓
KIRA kernel and HAL
```

Win32 и NT semantics не следует смешивать в один набор недифференцированных заглушек. Нужны отдельные модули API families и чёткая таблица покрытия.

## 3. Loader and PE requirements

- PE32/PE32+ parsing for explicitly supported machine types and subsystem versions.
- Validate DOS/PE headers, optional header, section table, alignment, image size, RVA/VA conversions, bounds and integer overflow.
- Map sections with correct permissions; enforce W^X and reject malformed/overlapping sections.
- Resolve imports, forwarded exports, ordinals, API-set contracts and DLL search order under a deterministic trusted policy.
- Apply base relocations, TLS callbacks, exception/unwind metadata and initial process environment where supported.
- Define manifest, activation context and side-by-side assembly policy for supported profiles.
- Reject unsupported PE features explicitly; never treat successful parsing as successful execution.

## 4. API and process semantics

Track coverage by Windows version/profile and architecture. Candidate API families:
- process/thread lifecycle, handles, waits and synchronization;
- virtual memory, section objects and mapped files;
- files, directories, file metadata and selected registry semantics;
- DLL loading, TLS, exceptions and unwind;
- sockets and networking;
- clocks, timers, locale and environment;
- graphics, windows, input, fonts and clipboard;
- audio and multimedia;
- installer/runtime dependencies and common redistributable libraries.

Use typed handles mapped to KIRA capabilities. Handle duplication, inheritance, access masks and close semantics must be explicit. Unsupported privileged NT operations fail closed.

## 5. Security model

- Each app runs in an isolated KIRA process/container with bounded capabilities.
- DLL search and import resolution must prevent DLL hijacking and untrusted library substitution.
- Filesystem, registry, network, graphics, audio and device operations are mediated by host services.
- No direct device access or kernel-driver compatibility in the user-mode runtime.
- Validate every pointer, buffer, handle, structure version and access mask at API boundaries.
- Track binary and library provenance, hashes, signatures where available, policy decisions and audit events.

## 6. Delivery phases

1. Choose an explicit Windows compatibility profile and target ISA.
2. PE/COFF loader and strict parser tests.
3. process startup, DLL imports and minimal kernel32/base APIs.
4. memory, files, handles, threads and synchronization.
5. exceptions/TLS, registry abstraction and sockets.
6. graphics/window/input/audio adapters.
7. packaging and selected installer/runtime dependencies.
8. real application matrix and security/performance hardening.

## 7. First vertical slice

Choose one small, redistributable Windows user-mode program with known dependencies. Demonstrate:
- verified PE load and import resolution;
- process/thread lifecycle;
- file access in an isolated virtual root;
- one synchronization primitive and one network or IPC operation;
- denied ungranted operation;
- malformed PE, missing DLL and invalid handle rejection;
- reliable process teardown.

A PE parser or API stub collection is not application compatibility.

## 8. Acceptance evidence

Record exact binary hash, application version, Windows profile, ISA, DLL closure, API coverage, test environment, expected/actual behavior, limitations, performance and security tests. x86-64 and ARM64 are distinct targets; cross-ISA translation is a separate project and is not implied by ARM64 support.
