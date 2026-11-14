# Stage 0 — current-main repository and build-profile inventory (2026-11-15)

**Current main:** `3497f48fe769dc61d90ee63da8cc7fc85d6eab7e`  
**Pinned baseline:** `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`  
**Audit branch:** `architecture/stage0-repository-audit-2026-11-14`  
**Source:** GitHub recursive Git trees and exact source/build files. This is a metadata and build-manifest audit, not a successful clean-room build.

## 1. Repository tree comparison

Both recursive trees were complete (`truncated=false`).

| Metric | Pinned baseline | Current main |
|---|---:|---:|
| Git tree entries (blobs + tree nodes) | 861 | 863 |
| Blob/file entries | 829 | 830 |
| `.c` | 593 | 593 |
| `.h` | 165 | 165 |
| `.md` | 16 | 17 |
| `.s` | 15 | 15 |
| `.sh` | 13 | 13 |
| `.py` | 11 | 11 |
| `.ld` | — | 4 |
| `.yml` | 3 | 3 |
| Other extensions | baseline snapshot | see exact current counts below |

Current-main extension counts: `c=593, h=165, md=17, s=15, sh=13, py=11, ld=4, yml=3, txt=2, out=2, gitignore=1, bin=1, rec=1, cfg=1, inc=1`.

### Exact path-level delta from pinned baseline

- Added: `docs/architecture/STAGE0_SOURCE_FINDINGS_2026-11-14.md` (documentation).
- Removed: **none**.
- Modified: `gfxsrv.c`, `kcc/kcc.h`, `userland/wasm.c`.
- Net source change is therefore limited to those three tracked source/header paths; all other tracked blob paths are unchanged relative to the pinned baseline.
- `gfxsrv.c`: per-app event queue lock, serialized event push/pop, app-slot teardown state.
- `kcc/kcc.h`: explicit `KCC_OK/KCC_ERROR/KCC_BUSY` result codes and documentation that compiler global tables prohibit concurrent/reentrant compilation in one process.
- `userland/wasm.c`: stricter bounded/overflow-checked LEB128 decoding and host-import signature validation, but the new signed-LEB 32-bit conversion uses undeclared `int32_t`/`uint32_t` names. This is the root of F-062 and current-main CI failure.
- Tree delta does not prove build artifact completeness or runtime correctness.

## 2. x86-64 build and distribution composition

Source: current-main `build_clang.sh`, `grub.cfg`, `LIMBO.md`, `.github/workflows/ci.yml`.

### Two-part Limbo delivery

The documented Android/Limbo output is a **pair**, not a standalone complete ISO:

1. `kira.iso`: GRUB boot files and `kira.bin` kernel.
2. `kira_disk.img`: KiraFS disk containing `/bin`, `/wasm`, compiler headers, driver package(s), trust roots and persistent system/user data.

The ISO staging directory copies only `kira.bin` and `grub.cfg` into `isodir/boot`; the companion disk is created/populated separately. `LIMBO.md` instructs users to attach both. Therefore an ISO-only artifact must not be described as the complete Limbo system payload.

### Release filtering

- Default/CI build keeps the full declared `UELF` list and CI uploads `kira.iso`, `kira.disk`, and `kira.bin`.
- `--android` or `KIRA_RELEASE=1` filters the `TESTPROGS` list from the x86-64 `/bin` payload before the manifest and disk are generated. The script comment explicitly describes these as test/educational programs; this is a profile distinction, not evidence that a core production subsystem is removed.
- The filter list is `burn mt ipc udpecho melody demo fpu atk-cap atk-mem atk-priv atk-sys atk-gs atk-jmp`. Every name must be compared with the actual compiled manifest before release; a name in the filter list is not proof that a corresponding binary was built.
- The test-only `drvedu` driver package is separately built and packed into `/drv` by `tools/mkdrv.sh` on the fresh build disk.
- On an existing Android disk, the update path removes only the listed `TESTPROGS`, then replaces current `UELF` binaries and updates WASM modules, KCC headers and roots only when a root bundle was found. It does not rebuild the whole disk image from scratch, and it does not update `/drv` in that branch. This is a persistent in-place update contract requiring explicit stale-file and package-update tests.
- The Android update path intentionally preserves user experience/data paths (`/var`, `/data`) as documented in `LIMBO.md`; any cleanup must preserve that state.

## 3. ARM64 build-profile differences

Source: current-main `build_arm64.sh`.

- ARM64 builds a separate `kira-arm64.img` kernel image and `kira-arm64.disk` KiraFS disk; `LIMBO_ARM.md` documents both.
- The current ARM64 script builds its listed user programs and writes them to the disk; it has no matching `TESTPROGS` release-filter block.
- The ARM64 initial application list differs from x86-64's explicit compile loops. It includes `atk-jmp` and does not mirror x86-64's exact `atk-gs` assembly list. These differences must be reconciled against source inventory, manifest and runtime capability—not guessed as parity or defects.
- ARM64 disk is initialized if present and current program files are written in place; stale files from removed/renamed programs are not explicitly pruned by the shown build script.
- This establishes a build-profile asymmetry; it does not establish physical ARM64 support.

## 4. CI evidence on current main

Current-main SHA `3497f48fe769dc61d90ee63da8cc7fc85d6eab7e`, push workflow `43643758138`:

| Job | Job ID | Result | Failure |
|---|---:|---|---|
| arm64 | 132596796555 | FAIL | `userland/wasm.c:145`, undeclared `int32_t`/`uint32_t` |
| host-tests | 132596796794 | FAIL | same source included by `tests/wasm_host.c`, undeclared `uint32_t` |
| build | 132596796963 | FAIL | same freestanding source error |
| vm | — | SKIPPED | prerequisite jobs failed |
| report | — | SUCCESS | reporting only; overall workflow remains FAILURE |

No new build was executed by this audit; the evidence above is from GitHub Actions logs for the exact current-main SHA.

## 5. Stage 0 follow-up required

- [ ] Generate and archive the actual full `UELF` manifests for x86 default, x86 Android/release and ARM64.
- [ ] Compare all three manifests by program name, SHA-256, ABI, source and purpose; classify test/educational/production/system apps.
- [ ] Verify the ISO file manifest and companion KiraFS disk manifest separately; never conflate them.
- [ ] Test a fresh disk build and an in-place Android update from an older disk containing stale binaries/packages; assert `/var` and `/data` are preserved.
- [ ] Verify `/drv` package update behavior on an existing Android disk.
- [ ] Verify WASM modules, KCC headers, trust roots, Mind seed, fonts/assets and any runtime-loaded resources are present and hash-recorded.
- [ ] Reconcile x86-64 and ARM64 program lists and explicitly record architecture-specific exclusions.
- [ ] Re-run current-main CI only after F-062 is fixed on a dedicated candidate branch; do not claim a green baseline before exact-head checks pass.

## 6. Evidence boundaries

This document is an exact source/tree/CI metadata inventory. No ISO or disk image was generated, extracted, hashed or booted in this audit pass. No claim is made that the current release pair is complete, reproducible, or hardware-ready.
