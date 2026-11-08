# KIRA Development Platform: drivers and powerful system programs

Status: architecture proposal for the KIRA Technology Lab.

## Goal

KIRA should not require every driver and system application to be written from zero. It should provide one coherent development environment in which a person or the built-in AI can create, port, compile, test, package and maintain software for KIRA.

## The development contract

Every component must declare:
- target architecture (x86-64, AArch64, later others);
- component type (kernel, driver service, system service, GUI application, compiler plugin, WASM module);
- required KIRA ABI/API version;
- memory and CPU budget;
- requested capabilities and device/resource ownership;
- dependencies and license identifiers;
- test plan and expected behavior.

## Proposed technology stack

### Languages and compilation

- C + assembly remain the boot and low-level hardware baseline.
- KCC remains the native KIRA compiler candidate and is tested against a conformance corpus.
- Clang/LLVM is the reference cross-compiler and optimization comparison baseline, not an automatic replacement.
- Rust is a candidate for new memory-sensitive services and parsers where target support and runtime footprint are proven.
- Zig is a measured candidate for low-level build and systems components, not a default.
- WASM/WASI-like sandboxing is considered for optional plugins, untrusted extensions and portable user applications, not hardware drivers that require direct MMIO/DMA.

### Intermediate representation and optimization

Test the current KCC IR against:
- typed SSA with explicit control-flow and memory effects;
- LLVM IR as a mature optimization/reference backend;
- target-specific lowering for x86-64 and AArch64;
- incremental compilation cache keyed by source digest, compiler version, target, flags, headers and dependency digests;
- LTO/PGO only where measured startup/runtime benefit outweighs build-time and binary-size cost.

Do not create a new general-purpose language yet. First create versioned interface schemas and a narrow driver-description DSL if it reduces errors.

### KIRA APIs and SDK

Publish versioned, documented interfaces:
- Driver ABI: device discovery, PCI/USB identity, MMIO mapping, IRQ waits, DMA buffers, reset and power lifecycle.
- System service API: typed IPC, capability handles, event subscriptions, shared-memory ownership and cancellation.
- Application API: windows, input, filesystem, networking, audio, notifications, accessibility and settings.
- Build API: target profiles, dependency manifests, deterministic flags and package metadata.

### AI-assisted development loop

1. User describes desired behavior or selects a source project.
2. AI reads the relevant KIRA API and examples, then creates a plan.
3. AI generates code or a small patch, not an unreviewable rewrite.
4. Compiler reports diagnostics; AI proposes a bounded correction.
5. Host tests, fuzz tests and static analysis run.
6. QEMU runs the component in a defined scenario.
7. Security broker checks capabilities and package provenance.
8. The user receives a plain-language report and can promote, keep quarantined, or reject the result.

The AI itself is not trusted to approve its own output. The compiler, tests, policy engine and release signer are separate gates.

## Documentation that must ship with KIRA

- Getting started: build and run the first program.
- Driver development guide: device IDs, bus model, MMIO, IRQ, DMA, lifecycle, power management, error handling.
- Linux porting guide: supported API map, unsupported calls, replacement examples and porting recipes.
- Application development guide: UI, filesystem, networking, IPC, permissions and packaging.
- Compiler guide: language subset, ABI, IR, diagnostics, optimization levels and debug symbols.
- Security guide: capabilities, isolation, threat model, DMA/IOMMU and signing.
- Testing guide: host models, fuzzing, QEMU scenarios, fault injection and physical-device validation.
- Reference cookbook: complete working examples, not pseudocode-only stubs.

## Quality bar

Every tutorial example must compile and run in CI. Every API page must point to a test or example. No placeholder function may be presented as a working API. The SDK version, compiler version, ABI version and test environment must be printed in every build report.
