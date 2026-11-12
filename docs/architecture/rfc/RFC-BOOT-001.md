# RFC-BOOT-001 — унифицированный BootInfo handoff

**Статус:** DRAFT — фактическая инвентаризация и варианты; не утверждённый ABI.  
**База:** main @ bbc7f7ab948e72511aed113b69043add10ec8350.

## 1. Фактическое состояние

- x86-64 common kernel entry: `kernel_main(unsigned long magic, unsigned long addr)`.
- Для Multiboot2 используется magic `0x36D76289`; `mbinfo_relocate()` переносит структуру выше 1 GiB через early boot map в статический буфер до 64 KiB.
- `mbinfo_find_tag()` обходит Multiboot2 tags с проверкой минимального размера/границ.
- `bootmap.h` определяет временное early physical mapping window на 1 GiB и предназначен для раннего доступа до PMM/VMM.
- ARM64 boot path в `arch/aarch64/main.c` использует FDT, memory nodes, chosen bootargs, UART, GIC/PSCI и платформенные данные.
- В main не обнаружен единый cross-ISA BootInfo structure/header. PR #42/#46 contain proposed handoff-related work but are not main.

## 2. Candidate implementation already present in an open branch (not main)

PR #42 branch `v46/kdrv-catalogue-signature-policy` contains a root `bootinfo.h` proposal:
- packed `kbi_header_t` size 136 bytes, magic `0x4941524B`, version 1;
- offset-based bounded blob with memory-map entries (24 bytes), module entries (32 bytes), command line, framebuffer channel layout, ACPI RSDP/FDT physical references, kernel physical base/size, architecture and boot mode;
- `bootinfo.c` validator, Multiboot2 adapter and FDT adapter;
- host tests for malformed ranges, overlap, module metadata, Multiboot2 tags and FDT parsing;
- kernel x86-64 and ARM64 entry paths consume this root KBI format in that branch.

However the same PR branch also adds a second incompatible `include/kira/bootinfo.h` format and `kernel/boot/boot_handoff.[ch]`; the latter is not in the production build source list or called by reviewed kernel entry paths. This duplicate must be resolved before any BootInfo ABI is accepted. The PR's boot implementation is not part of main.

## 3. Цель

Общий kernel bootstrap получает один версионированный, проверенный BootInfo view независимо от источника firmware/bootloader. Архитектурные различия остаются в backend adapters.

## 4. Proposed logical fields (not frozen layout)

- magic, structure_size, ABI_version, flags;
- architecture and firmware/boot protocol identity;
- physical memory map with type/attributes and overflow-safe ranges;
- kernel physical/virtual image ranges and entry metadata;
- command line as bounded byte span;
- framebuffer descriptor (optional);
- initrd/module descriptors (optional, each bounded);
- firmware table references (ACPI RSDP / FDT physical address + validated length);
- boot CPU/platform topology and interrupt-controller hints;
- entropy seed reference or explicit unavailable status;
- boot device/partition metadata where provided;
- reserved extension area and unknown-field policy.

Do not copy pointers from loader-owned structures into persistent kernel state without validation and ownership transfer.

## 5. Required invariants

1. All physical ranges are overflow-safe, aligned where required, non-overlapping unless explicitly allowed, and within declared memory/firmware map.
2. Kernel image, boot stack, BootInfo copy and modules are reserved before PMM exposes free pages.
3. Every pointer is represented as a bounded physical range or copied kernel-owned buffer; no unchecked loader pointer survives.
4. Unknown mandatory version/flags fail closed; unknown optional tags can be skipped only when size/alignment are valid.
5. BootInfo is immutable after handoff; services receive read-only views.
6. x86-64 and ARM64 adapters produce the same logical semantics, not necessarily identical raw firmware data.

## 6. Open decisions

- Whether to adopt the root `kbi_header_t` proposal from PR #42 as a candidate, after resolving duplicate format and reviewing every consumer, or redesign it based on this RFC.

- Whether bootloader-owned raw structures are normalized by each architecture backend or by a shared parser.
- Exact physical address width and memory attribute vocabulary.
- Whether ACPI and FDT coexist on ARM64 and precedence policy.
- Entropy quality/availability and secure-boot measurements.
- Optional framebuffer/module/boot-device ownership lifecycle.
- Version negotiation and compatibility window.

## 7. Conformance tests

- malformed magic/version/size/alignment;
- truncated tags, integer overflow and overlapping ranges;
- memory map with holes/reserved/MMIO regions;
- duplicate or conflicting firmware descriptors;
- FDT malformed property lengths and Multiboot2 malformed tag lengths;
- high physical address beyond early identity map;
- unknown optional and mandatory tags;
- identical logical memory/topology view for equivalent x86-64/ARM64 fixtures.

## 8. Acceptance gate

Do not freeze structure layout or replace existing boot paths until BIOS/UEFI x86-64 and ARM64 FDT boot adapters pass the same shared conformance suite and a migration path is documented.
