# KIRA OS — реестр открытых архитектурно значимых PR

**Снимок GitHub:** 2026-11-12  
**Базовая ветка main:** `bbc7f7ab948e72511aed113b69043add10ec8350`  
**Архитектурная ветка:** `architecture/target-baseline-2026-11`  
**Правило:** PR — отдельная ветка предложений, не часть main до merge. Статус CI проверять на текущем head SHA, не переносить старый PASS на новый commit.

## 1. Архитектурный PR

| PR | Head branch / SHA | Base | Направление | Следующая проверка |
|---|---|---|---|---|
| #135 | architecture/target-baseline-2026-11 / current head см. GitHub PR | main | Целевая архитектура, аудит, контракты | Проверить diff/docs/links и согласовать владельцем; draft, не merge |
| #136 | security/kdrv-require-iommu-for-dma-msi / current head см. GitHub PR | main | Block Ring 3 DMA/MSI without per-device IOMMU | Host/build/ARM64/QEMU CI running; review exact head before merge |
| #137 | security/pci-clear-firmware-busmaster / current head см. GitHub PR | main | Clear firmware bus-master before PCI ownership | Host/build/ARM64 CI running; review PCI mock test and exact head before merge |
| #136 | security/kdrv-require-iommu-for-dma-msi / 720a2020f6d78501aed60b5013ad9b7d1c62b30d | main | Fail-closed KDRV DMA/MSI isolation | Review security diff and require exact-head CI success; do not merge without review |

## 2. Основные открытые PR по подсистемам

| PR | Head branch / SHA | Base branch | Поток | Что требуется перед интеграцией |
|---|---|---|---|---|
| #134 | v46/kira-vm-core-v1 / aa5378d2a1cc52f9cbc55ab7432a4b0f7dad25a7 | v46/unified-release-integration | KIRA VM/KVP0 experiment | Сравнить с WASM baseline, проверить verifier/conformance/performance; не считать production |
| #132 | fix/vmm-kstack-double-free / 2b8569ad1baf8596e48c81ebebcdcefe8fae859d | v46/unified-release-integration | Kernel VMM stack lifecycle | Verify ownership, concurrent teardown, double-free tests, integration base |
| #131 | fix/wasm-module-header-bounds / 55328f649baad52f68a0cc5c39c166502eccf278 | main | WASM validation/host ABI | Parser fuzzing, import ABI, JIT W^X and ARM64 interpreter regression |
| #125 | fix/arm-smmu-integrated-hardening / cf35a78b385c0b9939702e19b097b01a4296e273 | autodrv | ARM SMMUv3/IOMMU | CI run #43207054075 failed: host-tests + ARM64 build at undeclared IOMMU_E_UNSUPPORTED; QEMU usb-uhci timed out. Fix and rerun before review |
| #120 | fix/arm-smmu-domain-encoding / 932f39a8a305da899e8616e3cd50eabb50400e82 | autodrv | SMMUv3 stage-1 domains | Workflow #42852157626 success; component evidence only. Reconcile with #125/#115/#116; no duplicate integration |
| #116 | feat/fdt-iommu-map / 6c37c98569986f088e184c2ce158c0a6dd49d1df | autodrv | FDT requester IDs/IOMMU | Workflow #43172445107 success; parser evidence only, not DMA isolation |
| #115 | fix/arm-smmu-stage1-context-descriptor-v11 / 680d01376cf43050471da867844dcb1cd5097df2 | autodrv | SMMUv3 address-width limits | Workflow #42822867501 success; component tests only, reconcile with #125/#120 |
| #112 | fix/bios-mbr-partition-cmdline-v2 / 2dcf255ff4832f5631b1105c7b05fb5fe872d4b6 | autodrv | BIOS boot handoff | Verify partition and cmdline parsing, image-content smoke |
| #95 | ai/kcc-context-more-state / b7cb6f655128b38f3b626da6a31f4fba35cad1df | ai/kcc-context-backend-regression | KCC compiler state isolation | Run #43211438660 failed kcc-tests self-host stage1 at ir.c:24 expected ')' before '->'; main build/host/ARM64/VM passed. Fix parser-compatible state access and add true concurrent-context tests |
| #93 | ai/kcc-context-backend-regression / 2f0e38254fbde12ff1a1673b27f0337690c319fb | ai/kcc-context-x86-state | KCC x86/AArch64 isolation | Run #43201182562 failed kcc-tests self-host stage1 at ir.c:14 expected ')' before K; sequential test only, no concurrent contexts |
| #81 | ai/mind-memory-pressure-forecast-v2 / 579e807f74efe21f9049c57a4111021a6f3ffec7 | ai/mind-validity-feature | Mind memory forecast | Run #43228045024 success: mind-quality, KCC, host, ARM64, driver, GUI and VM matrix all passed; package jobs skipped. Review model semantics and control safety before integration |
| #79 | perf/scheduler-sleep-heap / 5acdc5e359a434bfa1a14a198397b3189fb2b2f5 | autodrv | Scheduler performance | Validate wake ordering, cancellation, SMP races and benchmark baseline |
| #78 | ai/mind-validity-feature / 464070a038c35a22fa385395d6a47645f2caa489 | autodrv | Mind telemetry validity | Run #43201169922 success: mind-quality, KCC, host, ARM64, driver, GUI and VM matrix all passed; package jobs skipped |
| #74 | perf/scheduler-snapshot-linear / 5568c8f11dd8717ac19a4a17fcae07b5dfa13120 | autodrv | Scheduler snapshot | Lock hold time, consistency and concurrency stress |
| #72 | fix/bsd-driver-license-accuracy-rebased2 / 3b8f685caee564a8697ff6f4547e14a8d82066d8 | autodrv | Driver source licensing | Verify per-file provenance and license obligations |
| #50 | work/arch-parity-abi-v1 / 5ed9b14405239fc6ef585ab500ee2d65680fbf87 | autodrv | Syscall ABI x86-64/ARM64 | Compare proposal against existing syscall numbers/headers; avoid ABI collision |
| #49 | work/isolation-ipc-v1 / 9eae47fed27fc1d6bb0be9bb25b0b3fbdda7f64b | autodrv | IPC/capability isolation | Review object ownership, endpoint ACL, process boundary and negative tests |
| #46 | v46/unified-release-integration / dacc04d0dafd507ef0721698b5e9c2512f1947a5 | v46/kdrv-catalogue-signature-policy | Release/UEFI integration | Audit entire dependent release chain; no blind merge |
| #44 | v46/iommu-network-compile-fixes / b72a5b1f58bc1d8f585add9e3c05efa1a7cfd877 | v46/kdrv-catalogue-signature-policy | VT-d/SMMUv3/network | Reconcile dependency chain with KDRV and IOMMU PRs |
| #42 | v46/kdrv-catalogue-signature-policy / b08513f387d9ae9cc779f9faaa7e1eee26d02c39 | v46/kdrv-auth-integration-final | KDRV publisher policy | Check trust anchor, install/load policy and fail-closed |
| #41 | v46/kdrv-auth-integration-final / 5aefeb8cf1d0d13f8e9781c3cf5fe85bfedb2b8e | v46/release-completion | KDRV package signatures | Verify signed install atomicity, key identity and rollback |
| #40 | v46/kdrv-runtime-signature-import / 7504bda7cff2cafdd53b94f9faaa7e1eee26d02c39 | v46/release-completion | KDRV runtime signature import | Reconcile with #41/#42; prevent duplicate/competing implementation |
| #39 | v46/kdrv-signature-ci-routing / de5928260f8739d9a23e92a3d4367181e2ebc6cf | v46/ed25519-strict-subgroup | KDRV signature CI | Verify workflow coverage and actual tested head |
| #38 | v46/ed25519-strict-subgroup / fb12562b5ca07d3e19c2901db871f1e5fa91b0cd | v46/kdrv-host-link-fix | Crypto/Ed25519 | Known-answer, malformed key and signature negative tests |
| #36 | v46/kdrv-keyring-array / 03511e430e09d27869208c4eefe5eefc63988a54 | v46/kdrv-signature-work | KDRV keyring | Check key identity, lifecycle and keyring bounds |
| #35 | v46/kdrv-signature-work / d5b85234a518948911a7fcda486d0cc8e90c0891 | v46/dma-device-cleanup | KDRV signatures | Check provenance, signer policy and integration |
| #12 | v46/release-completion / c3342321667045a29b3c7e4726a31b926117a9b1 | v45/overhaul | Release completion | Review all dependent PRs, CI matrix and remaining blockers |

## 3. Branch topology risks

1. PR #132 currently has no associated PR-triggered workflow run for head `2b8569ad1baf8596e48c81ebebcdcefe8fae859d`; its helper tests do not exercise the actual VMM allocator under SMP or mapping-failure conditions.
2. PR #42 introduces two incompatible BootInfo v1 structures: root `bootinfo.h` (used by kernel/build) and `include/kira/bootinfo.h` (used by an unbuilt `kernel/boot/boot_handoff.c`). Resolve to one canonical ABI before integrating any boot changes.
3. Several important PRs target intermediate branches (`autodrv`, `v46/*`, `ai/*`) rather than main. Verify ancestry and cumulative diff; merging one PR is not equivalent to integrating its whole dependency chain.
4. IOMMU work (#115/#116/#120/#125/#44) overlaps ARM SMMUv3/FDT/VT-d. It needs one integration sequence and evidence for invalidation/fault lifecycle.
5. KDRV signature chain (#35/#36/#38/#39/#40/#41/#42/#44/#46/#12) has nested bases. PR #42 reports 95 changed files and mixes KDRV signature policy with boot, IOMMU, driver, test and release tooling. Its new signed-catalogue/trust/manager modules are host-test compiled but are not in production build source lists or called by kernel entry paths; the existing `kdrv.c` per-package signature path is a separate integration. ADR-0002 says strong anti-rollback is unavailable without hardware-backed monotonic storage. Split/reconcile the work and verify signer trust, key lifecycle, anti-rollback and end-to-end install/load.
6. KCC (#93/#95), Mind (#78/#81) and scheduler (#74/#79) are dependent pairs/chains; test cumulative results, not only isolated PRs.
7. #134 is an experimental VM; current KVR production baseline remains WASM under the master plan/ADR-0001. Its head SHA `aa5378d2a1cc52f9cbc55ab7432a4b0f7dad25a7` workflow `43236030817` failed: host-tests lint flagged `fake_ticks` as a stub; ARM64 and VM shards 5/7/9 also failed. Other VM shards, build, UEFI OVMF, KCC and driver E2E succeeded. Do not promote KVM1 on this evidence.

## 4. Остальные открытые PR

В доступной выборке GitHub на снимке было 47 открытых PR. Остальные направления включают release/CI factories, boot/UEFI, USB/audio/network drivers, compiler research, technology decisions, GUI/driver manager documentation and validation research. Их нужно включить в полный tracker при следующем PR-by-PR review; этот документ перечисляет наиболее критичные архитектурные зависимости и не объявляет остальные неважными.

## 5. Правила обновления

Перед любым решением по PR повторно получить:
- текущие head/base refs и SHAs;
- changed files and cumulative diff;
- review threads and unresolved comments;
- workflow runs and job conclusions for exact head SHA;
- branch protection/mergeability;
- зависимые PR и текущую main ancestry.

Не merge/close PR без явного разрешения владельца.
