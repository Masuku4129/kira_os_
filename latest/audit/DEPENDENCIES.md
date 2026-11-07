# KIRA OS — аудит зависимостей

Статический анализ исходников и build scripts. Он не заменяет бинарный анализ ISO/KiraFS и security review.

- Tracked files: 852; production C/ASM/header files scanned: 351.
- OS build linker -l flags: 1; host-test-only linker flags: 2 (m, pthread); library-file candidates: 0.
- Git submodules: no; vendor directories: 0.
- Freestanding/no-libc evidence in build flags: not confirmed.

## Components and tools

| Component | Classification |
|---|---|
| GRUB 2 / Multiboot2 | Bundled in x86 ISO; external boot-chain dependency |
| Shared/static library files | No tracked library files detected; binary image scan still required |
| CA roots | Host CA bundle copied to /etc/ssl/roots.der (trust data) |
| clang | build-time compiler |
| ld.lld | build-time linker |
| nasm | build-time x86 assembler |
| wasm-ld | build-time WebAssembly linker |
| llvm-objcopy | build-time ARM image converter |
| grub-mkrescue | build-time ISO tool; GRUB itself is included in the ISO |
| xorriso | build-time ISO authoring tool |
| qemu-system-x86_64 | test-only virtual x86 machine |
| qemu-system-aarch64 | test-only virtual ARM machine |
| qemu-system-arm | test-only virtual ARM machine |
| gcc | host-side build/test compiler |
| python3 | CI/test orchestration |
| openssl | test oracle, not an OS library |
| python3-cryptography | test oracle, not an OS library |
| python3-pil | test image helper, not an OS library |
| dosfstools | host-side FAT test utility |
| mtools | host-side FAT image utility |
| ca-certificates | trust-root data copied to the system disk |
| KVM | host acceleration for QEMU tests; not shipped in the OS image |

## Source provenance markers

- drivers/rl/if_rlreg.h:4: * Основано на FreeBSD sys/dev/rl/if_rlreg.h (rl(4)): имена регистров и битов оставлены прежними, чтобы порт читался рядом с оригиналом.
- drivers/rl/rl.c:3: * Драйвер RealTek RTL8139 (10ec:8139) для Киры, Ring 3 / EL0: порт rl(4) из FreeBSD (sys/dev/rl/if_rl.c) на слой kdrv_bsd.h.
- kcc/include/kdrv_bsd.h:4: /* Слой совместимости для портов BSD-драйверов (FreeBSD, OpenBSD, NetBSD) в драйвер Ring 3 Киры. Не вся ядерная часть BSD, а ровно то,
- userland/kdrv_bsd.h:4: /* Слой совместимости для портов BSD-драйверов (FreeBSD, OpenBSD, NetBSD) в драйвер Ring 3 Киры. Не вся ядерная часть BSD, а ровно то,

## Remaining independence/security blockers

- Replace GRUB/Multiboot2 with KIRA's own BIOS and UEFI loader.
- kcc supports a C subset; full build still uses external Clang/LLD/NASM and wasm-ld.
- RTL8139 rl(4) is a BSD-attributed port based on FreeBSD driver behavior; a specification-based rewrite is required for a strict all-original-code policy.
- SHA-256 alone does not authenticate package publishers; add signed manifests, trusted keys and anti-rollback checks.
- DMA-capable drivers need IOMMU domains or DMA must be disabled for untrusted drivers.
- QEMU results do not validate physical x86/ARM hardware.

## Scanner limits

- Include resolution and linker flags are heuristic; generated headers and compiler-provided headers are classified separately.
- Host-test linker flags and tools are inventoried separately; they are not linked into the OS image.
- The script does not unpack final ISO or KiraFS images. Run a binary-level audit on the produced artifacts before release.
- Provenance markers require human review; they are not a license verdict.
