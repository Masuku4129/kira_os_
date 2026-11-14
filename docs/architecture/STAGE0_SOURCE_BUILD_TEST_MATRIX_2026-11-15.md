# KIRA OS — source / build / test coverage matrix (Stage 0)

**Audited source ref:** `main` @ `3497f48fe769dc61d90ee63da8cc7fc85d6eab7e`  
**Audit ref:** `architecture/stage0-repository-audit-2026-11-14`  
**Method:** current-main source files and `tests/run_host_tests.sh` were inspected. Test entries below mean the test is wired into the host runner; they do not mean the latest runner passed. Current-main run `43643758138` stops at the WASM compile blocker F-062 before the later tests can execute.

## Coverage matrix

| Subsystem / contract | Production source | Build integration | Host test / evidence | Stage 0 assessment |
|---|---|---|---|---|
| Boot map / early MMIO | `bootmap.c/.h`, x86 boot + `mbinfo.c` | x86 kernel source list; ARM64 path requires separate review | `tests/bootmap_test.c` | Host test exists; hardware mapping and architecture parity not proven |
| Physical memory manager | `pmm.c`, `ramext.c` | x86 + ARM64 source lists | `pmm_test.c`, `ramext_test.c` | Host coverage exists; current-main runtime/CI baseline blocked |
| Virtual memory / address spaces | `vmm.c`, arch VMM code | x86 + ARM64 | no direct standalone VMM host test identified in runner | Critical uncovered host-test area; QEMU isolation tests exist in workflow but are skipped on current main |
| Scheduler / context switch | `sched.c`, `scale.c`, arch context | x86 + ARM64 | `sched_test.c`, `ctx_fpu_test.c`, `scale_test.c` | Host coverage exists; multicore race/soak requires successful QEMU jobs |
| Process / syscall | `proc.c`, `syscall.c`, arch syscall/uaccess | x86 + ARM64 | `prog_test.c`, `corefault_test.c`; QEMU process/isolation scenarios | Process-level coverage exists; syscall ABI matrix is not separately enumerated in host runner |
| Capability policy | `caps.c/.h`, `trust.c` | x86 + ARM64 | `fsrv_test.c`, `prog_test.c` indirect | No dedicated exhaustive capability bit/identity matrix identified |
| ELF loader validation | `elf.c/.h`, `proc.c` | x86 + ARM64 | `prog_test.c` compiles `elf.c` | Need explicit malformed ELF corpus, overflow boundary, segment overlap, alignment, null-output tests |
| KiraFS | `kfs.c/.h`, `store.c` | x86 + ARM64; host kfstool | `kfs_test.c`, `fat_test.c`, `store_test.c` | Broad host tests wired; current CI runner did not reach later tests |
| Block/storage drivers | `blk.c`, ATA/AHCI/NVMe/virtio-blk | x86; ARM64 profile differs | `virtio_test.c`, `kfs_test.c` | Need device error/flush/short-I/O matrix and QEMU disk profiles |
| FDT / firmware device tree | `fdt.c/.h`, ARM64 boot consumers | ARM64 kernel | `fdt_test.c` | Existing fixture is v17 only; missing v16 compatibility, strings-block boundary/NUL, malformed nameoff, reserve-map coverage |
| PCI / DMA / driver framework | `pci.c`, `kdrv.c`, `kdrv_fmt.c`, driver ABI | x86 and selected ARM64 components | `kdrv_test.c`, `drvget_test.c`, `drvget_e2e.sh`, `rl_build.sh` | IOMMU/device ownership and failure rollback require end-to-end/QEMU evidence |
| USB | `usb.c`, `usbhub.c`, `usbmsc.c`, UHCI/XHCI | x86 | `usb_test.c`, `uhci_test.c`, `xhci_test.c` | Host tests wired; actual hotplug/controller QEMU jobs must pass |
| Network / TCP / IPv6 | `net.c`, `ip6.c`, `ipfrag.c`, drivers | x86 + selected ARM64 | `net_test.c`, `ip6_test.c`, `ipfrag_test.c` | Host tests exist; PR stack includes lifecycle fixes with stale base dependencies |
| TLS / X.509 / crypto | `userland/tls.c`, `userland/x509.c`, `pk.c`, `crypto.c`, `sha256.c` | x86 userland and ARM64 userland | `crypto_test.c`, `pk_test.c`, `x509_test.c`, `tls_host.c` | Host coverage exists; certificate policy and reentrancy PRs need ancestry/CI verification |
| WASM runtime | `userland/wasm.c`, build-wasm modules | x86 + ARM64 | `wasm_host.c`, differential/fuel scripts | Current-main hard build blocker F-062 prevents runner completing; malformed module fuzz coverage still needs review |
| KCC compiler | `kcc/*.c`, `userland/kcc.c` | x86 + ARM64 | `tests/kcc/run.sh`, fuzz/IR/exec corpus | Large corpus exists; target-specific and reentrancy contracts need separate evidence |
| Graphics / window manager | `gfx2d.c`, `gfxui.c`, `wm.c`, `display.c`, `gfxsrv.c` | x86 + selected ARM64 | `gfx_test.c`, `ui_test.c`, `wm_test.c`, `display_preview.c`, `dock_test.c` | Host rendering/UI tests exist; full interactive QEMU scenarios require passing CI |
| KIRA Mind | `mind.c`, `mindnet.c`, `mind_sys.c`, `experience.c` | x86 + ARM64 | `mind_test.c`, `mind_lever_test.c`, `mindnet_test.c` | Host tests wired; seed/resource parity still requires image manifest verification |
| Audio | `audio.c`, `hda.c`, `ac97.c`, `sb16.c` | x86 + selected ARM64 | `audio_test.c`, `sb16_test.c` | PR #180 proposes stream-lifetime fix; base is stale relative to current main and requires rebase/retest |
| Runtime user apps | `userland/*.c`, `progs.c`, `build_clang.sh`, `build_arm64.sh` | Distinct x86/ARM64 app manifests | ELF manifest + KFS tests; QEMU app scenarios | No checked-in canonical generated manifest for each release profile; profile parity remains open |
| Installer / update / recovery | `build_clang.sh --android`, `tools/kfstool.c`, `LIMBO.md` | x86 Android profile | no end-to-end host test for old-disk update path identified | Critical release coverage gap: stale-file cleanup, package update, roots absence, /var and /data preservation |
| Driver adaptation / supply chain | `userland/drvget*.c`, `kdrv_fmt.c`, `docs/DRIVERS.md` | x86 and ARM64 driver build path to reconcile | `drvget_test.c`, `drvget_e2e.sh` | Need source provenance, license, signing, sandbox, rollback and no-network-binary execution evidence |

## Exact host-runner coverage

The current `tests/run_host_tests.sh` wires the following groups (not all are independent test binaries): logic, heap, memory primitives, PMM, RAM extensions, scheduler, FPU/context switch, input, driver framework, driver fetch, Realtek driver build, ACPI, IOAPIC, store, KiraFS, virtio, kfstool/program manifest, process/ELF/trust, fsrv/capabilities, paint, spinlocks, mutexes, kernel objects/futex, core faults, scaling, SMP, user logs, boot map, early boot/console/crash dump, user heap, hardware profile, 3D/FPU, graphics/console/UI/window manager/display/dock, Mind, FDT, crypto, RTC, file manager, USB stack/controllers, FAT/VFS, view, WASM interpreter/differential/fuel, audio, SB16, network/IPv6/fragmentation, public-key/X.509/TLS, drvget E2E and KCC suite.

The runner's final success marker is `ALL HOST TESTS PASSED`. A successful reporting step or a subset of earlier commands is not equivalent to this final marker.

## Priority coverage gaps for Stage 0 closure

1. Add a dedicated VMM/address-space host harness or document why QEMU is the only viable test boundary; include page permissions, canonical addresses, guard pages, teardown and cross-CPU TLB invalidation.
2. Expand FDT tests with v16, strings bounds/NUL termination, property-name offset edge cases and reserve map parsing.
3. Expand ELF loader corpus with overflow edges and fuzzing; make malformed segment and entry-point expectations explicit.
4. Add release-profile manifest snapshots for x86 default, x86 Android and ARM64; compare names, hashes, ABI and purpose.
5. Add destructive-update integration tests on a copied disk image: stale binaries, removed drivers, missing CA roots, interrupted writes, /var and /data preservation.
6. Map each QEMU workflow scenario to the subsystem acceptance contract and exact required job; distinguish x86, ARM64, Limbo and physical hardware.
7. Add per-architecture build coverage table for every source file: compiled, excluded intentionally, or unsupported with documented reason.

## Evidence boundary

This matrix is a wiring/coverage audit. It does not assert that any host test or guest scenario passed on current main. Current main's exact-head CI run `43643758138` fails x86 build, ARM64 build and host-tests on F-062; QEMU is skipped.
