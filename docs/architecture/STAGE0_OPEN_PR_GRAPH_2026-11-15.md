# Stage 0 — полный снимок открытого PR-графа (2026-11-15)

**Repository:** `Masuku4129/kira_os_`  
**Снимок:** GitHub REST `GET /pulls?state=open&per_page=100`, получен в рамках Stage 0.  
**main:** `3497f48fe769dc61d90ee63da8cc7fc85d6eab7e`  
**Audit branch:** `70b0290d837ed14bf605f2cb7982707feb5c2293` на момент снимка.  
**Открытых PR:** 92. Это снимок, не постоянная истина: перед интеграцией перечитать PR и workflow на актуальных SHA.

## Ключевые выводы

1. Открытые PR образуют не одну линейную очередь, а несколько независимых стеков и исторических веток. Нельзя сливать их по номеру или считать все PR основанными на текущем `main`.
2. В графе присутствуют ветки с разными базовыми линиями: текущие исправления около #151–#182; Spatial/UI стек #138–#150; старые `autodrv` и `v46/*` цепочки; отдельные research/AI/toolchain ветки.
3. В PR #179/#181/#182 зафиксирован общий build blocker F-062. Эти PR stacked; исправление/CI родителя обязательно до оценки потомков.
4. В PR #150, #149, #145 и #138–#146 есть отдельная UI/release цепочка, основанная не на `main`. Не переносить в новый Stage план как готовое production-состояние без сравнения дерева и полного набора артефактов.
5. Само наличие PR и зелёный CI на другом SHA не закрывает дефект в `main`. Каждому кандидату нужны diff, ancestry, конфликтный анализ и exact-head CI.

## Группировка по base ref + base SHA

- `main @ 811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`: 23 PR — #173, #172, #171, #170, #168, #167, #166, #165, #164, #162, #161, #160, #159, #158, #157, #156, #155, #154, #153, #152, #151, #148, #138
- `autodrv @ 10a19683fa11f8afea77742067f2e4498e6cacad`: 6 PR — #116, #115, #112, #78, #76, #74
- `v46/kdrv-catalogue-signature-policy @ 796a5a0d66f4ca60637af81c092738aebdd415e7`: 4 PR — #46, #45, #44, #43
- `main @ bbc7f7ab948e72511aed113b69043add10ec8350`: 3 PR — #137, #136, #135
- `autodrv @ 6a66ebf6f45b367e40a170bf565585f07af1a985`: 3 PR — #121, #120, #72
- `autodrv @ d7f0d08a5272ea06b67b9b6684f14f22c4bced5d`: 3 PR — #57, #55, #54
- `main @ 12cf3fb8e05b3d04c0b08e9cc9aeaad750214f2d`: 2 PR — #180, #179
- `main @ 1944bc6063371c715339b52784b5a206358a48de`: 2 PR — #177, #176
- `main @ 8a9fd97fd84ff1c003142d208f19fe9d0170bf26`: 2 PR — #175, #174
- `feat/spatial-visual-tokens-2026-11-13 @ fd9ae60dc4f0f332ae917854ca272ff729131d43`: 2 PR — #150, #149
- `feat/spatial-minimal-desktop-visual-2026-11 @ 95a672eae2ef94a61eb29df60e509d69ce5c4335`: 2 PR — #146, #145
- `autodrv @ 79d2d84e1d0fcd396f64b67e1752790053cb6d33`: 2 PR — #59, #58
- `main @ c63520432ea767844e0f04a7ab2a827e69d89b93`: 2 PR — #48, #47
- `v46/release-completion @ bc98b6f07ca10ee70181851a39fc8fbf8db1c6ac`: 2 PR — #41, #40
- `v46/architecture-completion @ 813b1de9cf0a2d079bfeb08b3b20ada76347afba`: 2 PR — #23, #22
- `fix/wasm-bounds-and-import-validation-2026-11-15 @ 2fdb2c04fc3d5a938c547ad3b8285bebd1a7f736`: 1 PR — #182
- `fix/wasm-bounds-and-import-validation-2026-11-15 @ eb52e6e3055c38a8ea32f65eb12d3f2c0924c132`: 1 PR — #181
- `main @ d2557ad35a9e3b1e31944bb0d1fedd77539fdb9e`: 1 PR — #178
- `security/pci-firmware-busmaster-main-2026-11-14 @ 7ad140cfadb51c4dacf809eca77fff60ff7fe12c`: 1 PR — #169
- `baseline/spatial-preview-5fc8f491 @ 5fc8f49184e2e27e44790bdcd238977bfd126cc4`: 1 PR — #144
- `feat/spatial-context-panel-2026-11 @ 10650832ad60d3d1cf871c53a675dda2b28136fc`: 1 PR — #143
- `fix/ui-spatial-damage-bounds-2026-11 @ efada8f2d64c55516ac6ac76f41b58a1bf1cad16`: 1 PR — #142
- `architecture/pci-range-device-model-2026-11 @ ea83f83bdc59307bba22a6bd013b24c5c5c09731`: 1 PR — #141
- `docs/ui0-spatial-minimal-audit @ c05dac32ac9ced00e8e8856f2520012e3ef23128`: 1 PR — #140
- `architecture/target-baseline-2026-11 @ 78a76003d0d808147f994c7ed23098c607039fd8`: 1 PR — #139
- `v46/unified-release-integration @ 87f65b3f49cb5b5402e47080ae44cf1926cf94c6`: 1 PR — #134
- `v46/unified-release-integration @ e9fb1151a50d1c59d4422829ee23468cd04341a0`: 1 PR — #132
- `main @ 8d2904061423ba7936926fcfd64cd1659420a406`: 1 PR — #131
- `autodrv @ e2c55cec08a972187953dce17fcb0e231e382c1a`: 1 PR — #125
- `ai/kcc-context-backend-regression @ 44396779377c1eb82ca96fbefe8bc580b5276c1f`: 1 PR — #95
- `ai/kcc-context-x86-state @ ace418857a25ec473594b40d5a5995aa7dd4ee6d`: 1 PR — #93
- `ai/mind-validity-feature @ 7028f117ab6cc7f9523e45bacbd7a6513decf6fb`: 1 PR — #81
- `autodrv @ c75e01bb6144a04e2c4fb98d28ec75a0ff55cf9e`: 1 PR — #79
- `autodrv @ 19b5a0662912410a5a9ba9c46f7283a64e0534ef`: 1 PR — #63
- `autodrv @ 234c83b0e51391968b7140f653936e57caf1f3ad`: 1 PR — #52
- `autodrv @ 1a388c2913e970caaee42f1ae0db87b9a4579d35`: 1 PR — #51
- `autodrv @ 1c8980b0f6ffcfbb456c8a829ba6cea1934833fa`: 1 PR — #50
- `autodrv @ b005226ad24616adb459974510bc75296a524ac8`: 1 PR — #49
- `v46/kdrv-auth-integration-final @ 7504bda7cff2cafdd53b94f9f922698238f76a21`: 1 PR — #42
- `v46/ed25519-strict-subgroup @ fb12562b5ca07d3e19c2901db871f1e5fa91b0cd`: 1 PR — #39
- `v46/kdrv-host-link-fix @ 39427ad9a8b56a476042ece2ffd9399942a45eb6`: 1 PR — #38
- `v46/kdrv-signature-work @ d5b85234a518948911a7fcda486d0cc8e90c0891`: 1 PR — #36
- `v46/dma-device-cleanup @ 152a17a82c8a9992bc3de2b6cf5b2fbdb40257c2`: 1 PR — #35
- `work/ci-drvman-log-fix @ 11582736308187700ac7b1f3ae5cf648046a2f37`: 1 PR — #32
- `work/kdrv-auth @ 31d186176f69efa7bd2df4c91d41e1d851c6ffce`: 1 PR — #24
- `v46/architecture-completion @ 03735915ada00edc567afc5e2eda51c3ed10028a`: 1 PR — #21
- `v45/overhaul @ c07a7a3465fa8d3ed88b4b2478fa9955b0a756ca`: 1 PR — #12

## Полный реестр открытых PR

| PR | Заголовок | Head branch | Head SHA | Base branch | Base SHA | Статус |
|---:|---|---|---|---|---|---|
| [#182](https://github.com/Masuku4129/kira_os_/pull/182) | fix(gfx): publish reused app slots under queue lock | `fix/gfx-event-queue-serialization-followup-2026-11-15` | `49e2914e620e` | `fix/wasm-bounds-and-import-validation-2026-11-15` | `2fdb2c04fc3d` | draft |
| [#181](https://github.com/Masuku4129/kira_os_/pull/181) | fix(input): synchronize producer and consumer ring state | `fix/input-ring-atomic-ownership-2026-11-15` | `17614b29e4bb` | `fix/wasm-bounds-and-import-validation-2026-11-15` | `eb52e6e3055c` | draft |
| [#180](https://github.com/Masuku4129/kira_os_/pull/180) | fix(audio): pin stream handles across writes and teardown | `fix/audio-stream-lifetime-2026-11-15` | `2b55098b4605` | `main` | `12cf3fb8e05b` | draft |
| [#179](https://github.com/Masuku4129/kira_os_/pull/179) | fix(wasm): validate host imports and malformed LEB encodings | `fix/wasm-bounds-and-import-validation-2026-11-15` | `2fdb2c04fc3d` | `main` | `12cf3fb8e05b` | draft |
| [#178](https://github.com/Masuku4129/kira_os_/pull/178) | fix(tcp): synchronize socket waits and pin active lifetimes | `fix/tcp-lifecycle-locking-2026-11-14` | `b60fbc0c761d` | `main` | `d2557ad35a9e` | draft |
| [#177](https://github.com/Masuku4129/kira_os_/pull/177) | fix(wasm): bound module decoding and validate imported ABI | `fix/wasm-bounded-decoder-2026-11-14` | `b86ed6b2017b` | `main` | `1944bc606337` | draft |
| [#176](https://github.com/Masuku4129/kira_os_/pull/176) | fix(kcc): reject concurrent compiler entry safely | `fix/kcc-single-active-guard-2026-11-14` | `6ff41ceb5fe0` | `main` | `1944bc606337` | draft |
| [#175](https://github.com/Masuku4129/kira_os_/pull/175) | fix(tls): isolate handshake state per connection | `fix/tls-x509-reentrancy-2026-11-14` | `4c7f7562d30d` | `main` | `8a9fd97fd84f` | draft |
| [#174](https://github.com/Masuku4129/kira_os_/pull/174) | fix(x509): enforce critical extensions and certificate constraints | `fix/x509-critical-constraints-2026-11-14` | `d8e20596ca41` | `main` | `8a9fd97fd84f` | draft |
| [#173](https://github.com/Masuku4129/kira_os_/pull/173) | fix(net): generation-tag TCP handles and pin connection lifetimes | `fix/tcp-lifecycle-generation-2026-11-14` | `aa6e7832d93a` | `main` | `811ca3a44ce0` | draft |
| [#172](https://github.com/Masuku4129/kira_os_/pull/172) | fix(crypto): enforce RFC 5869 HKDF limits without truncation | `fix/hkdf-rfc5869-bounds-2026-11-14` | `3fda59b30cc2` | `main` | `811ca3a44ce0` | draft |
| [#171](https://github.com/Masuku4129/kira_os_/pull/171) | Fix UDP socket lifetime and stale handles | `fix/udp-socket-lifetime-2026-11-14` | `962156677402` | `main` | `811ca3a44ce0` | draft |
| [#170](https://github.com/Masuku4129/kira_os_/pull/170) | Fix UDP transmit error propagation | `fix/udp-transmit-error-propagation-2026-11-14` | `fe53dff63b96` | `main` | `811ca3a44ce0` | draft |
| [#169](https://github.com/Masuku4129/kira_os_/pull/169) | fix(usb): defer Bus Master until controllers and DMA state are ready | `fix/usb-dma-order-and-xhci-slot-retry-2026-11-14` | `0080dc4e2b15` | `security/pci-firmware-busmaster-main-2026-11-14` | `7ad140cfadb5` | draft |
| [#168](https://github.com/Masuku4129/kira_os_/pull/168) | ci: split QEMU guest errors at KIRA ready boundary | `ci/qemu-phase-diagnostics-2026-11-14` | `7ad4b30cf9e7` | `main` | `811ca3a44ce0` | draft |
| [#167](https://github.com/Masuku4129/kira_os_/pull/167) | fix(sched): combine guarded stack ownership and fail-closed allocation | `integration/kernel-stack-pmm-combined-2026-11-14` | `276307799849` | `main` | `811ca3a44ce0` | draft |
| [#166](https://github.com/Masuku4129/kira_os_/pull/166) | fix(fdt): reserve firmware memory ranges from the header map | `fix/fdt-reserve-map-2026-11-14` | `4bc69a224887` | `main` | `811ca3a44ce0` | draft |
| [#165](https://github.com/Masuku4129/kira_os_/pull/165) | security(pci): clear firmware bus mastering before device ownership | `security/pci-firmware-busmaster-main-2026-11-14` | `7ad140cfadb5` | `main` | `811ca3a44ce0` | draft |
| [#164](https://github.com/Masuku4129/kira_os_/pull/164) | fix(sched): fail closed on guarded stack exhaustion and clear stack memory | `fix/scheduler-guarded-stack-only-2026-11-14` | `3729347c19f8` | `main` | `811ca3a44ce0` | draft |
| [#162](https://github.com/Masuku4129/kira_os_/pull/162) | fix(pci): honor host bus range and bridge subordinate windows | `fix/pci-host-bus-range-bounds-2026-11-14` | `2e30dc5c9edf` | `main` | `811ca3a44ce0` | draft |
| [#161](https://github.com/Masuku4129/kira_os_/pull/161) | security(kdrv): block Ring 3 DMA/MSI without per-device IOMMU | `security/kdrv-iommu-policy-main-2026-11-14` | `29a654abe16f` | `main` | `811ca3a44ce0` | draft |
| [#160](https://github.com/Masuku4129/kira_os_/pull/160) | fix(vmm): check MMIO overflow with atomic UART logging | `integration/mmio-uart-logging-2026-11-14` | `d5628989db14` | `main` | `811ca3a44ce0` | draft |
| [#159](https://github.com/Masuku4129/kira_os_/pull/159) | fix(fdt): reject wrapping reg ranges and indices | `fix/fdt-reg-range-overflow-2026-11-14` | `1a61bd5bdc11` | `main` | `811ca3a44ce0` | draft |
| [#158](https://github.com/Masuku4129/kira_os_/pull/158) | fix(arm64): protect reserved pages and make PMM block release atomic | `fix/arm64-pmm-ownership-atomic-free-2026-11-14` | `70d0e89a6541` | `main` | `811ca3a44ce0` | draft |
| [#157](https://github.com/Masuku4129/kira_os_/pull/157) | fix(uart): keep long driver log records atomic | `fix/uart-atomic-long-lines-2026-11-14` | `59b391696baa` | `main` | `811ca3a44ce0` | draft |
| [#156](https://github.com/Masuku4129/kira_os_/pull/156) | fix(vmm): reject overflowing MMIO page ranges | `fix/mmio-range-overflow-2026-11-14` | `cbad3a219874` | `main` | `811ca3a44ce0` | draft |
| [#155](https://github.com/Masuku4129/kira_os_/pull/155) | fix(smp): make x86 TLB shootdown fail closed | `fix/x86-tlb-shootdown-fail-closed-2026-11-14` | `f2df3472d782` | `main` | `811ca3a44ce0` | draft |
| [#154](https://github.com/Masuku4129/kira_os_/pull/154) | fix(memory): validate kernel stacks and make PMM block release atomic | `integration/kernel-memory-foundation-2026-11-14` | `bfdffa64555a` | `main` | `811ca3a44ce0` | draft |
| [#153](https://github.com/Masuku4129/kira_os_/pull/153) | fix(pmm): make contiguous page release atomic | `fix/pmm-atomic-contiguous-release-2026-11-14` | `861707fc4538` | `main` | `811ca3a44ce0` | draft |
| [#152](https://github.com/Masuku4129/kira_os_/pull/152) | fix(vmm): harden kernel stack slot ownership on x86-64 and ARM64 | `fix/kernel-stack-slot-lifecycle-2026-11-14` | `48518fac18ba` | `main` | `811ca3a44ce0` | draft |
| [#151](https://github.com/Masuku4129/kira_os_/pull/151) | docs(architecture): start bottom-up Stage 0 repository audit | `architecture/stage0-repository-audit-2026-11-14` | `70b0290d837e` | `main` | `811ca3a44ce0` | draft |
| [#150](https://github.com/Masuku4129/kira_os_/pull/150) | Spatial Glass desktop: production default and unified window controls | `feat/spatial-glass-complete-2026-11-14` | `6c3d143d6838` | `feat/spatial-visual-tokens-2026-11-13` | `fd9ae60dc4f0` | draft |
| [#149](https://github.com/Masuku4129/kira_os_/pull/149) | Add full Spatial Android build mode with disk preservation | `feat/android-full-release-build-2026-11-14` | `77f99d3a0cfd` | `feat/spatial-visual-tokens-2026-11-13` | `fd9ae60dc4f0` | ready |
| [#148](https://github.com/Masuku4129/kira_os_/pull/148) | docs: add three-stage project status and file-loss guard | `project/status-ledger-2026-11-13` | `bf35ca714526` | `main` | `811ca3a44ce0` | draft |
| [#146](https://github.com/Masuku4129/kira_os_/pull/146) | ui(spatial): apply approved visual design tokens across shell and apps | `feat/spatial-visual-tokens-2026-11-13` | `fd9ae60dc4f0` | `feat/spatial-minimal-desktop-visual-2026-11` | `95a672eae2ef` | draft |
| [#145](https://github.com/Masuku4129/kira_os_/pull/145) | ci(release): publish complete x86-64 and ARM64 system images | `release/full-os-artifacts-2026-11-13` | `4b28ee5fb60e` | `feat/spatial-minimal-desktop-visual-2026-11` | `95a672eae2ef` | draft |
| [#144](https://github.com/Masuku4129/kira_os_/pull/144) | feat(gui): complete Spatial Minimal desktop visual pass | `feat/spatial-minimal-desktop-visual-2026-11` | `95a672eae2ef` | `baseline/spatial-preview-5fc8f491` | `5fc8f49184e2` | draft |
| [#143](https://github.com/Masuku4129/kira_os_/pull/143) | Spatial workspaces and contextual notifications | `feat/spatial-workspaces-notifications-2026-11` | `5fc8f49184e2` | `feat/spatial-context-panel-2026-11` | `10650832ad60` | draft |
| [#142](https://github.com/Masuku4129/kira_os_/pull/142) | feat(gui): add on-demand Spatial system status panel | `feat/spatial-context-panel-2026-11` | `10650832ad60` | `fix/ui-spatial-damage-bounds-2026-11` | `efada8f2d64c` | draft |
| [#141](https://github.com/Masuku4129/kira_os_/pull/141) | feat(device-model): add common registry and PCI discovery adapter | `architecture/device-registry-pci-adapter-2026-11` | `cda2ba2b3b94` | `architecture/pci-range-device-model-2026-11` | `ea83f83bdc59` | draft |
| [#140](https://github.com/Masuku4129/kira_os_/pull/140) | fix(gui): restore partial redraw performance in Spatial Minimal | `fix/ui-spatial-damage-bounds-2026-11` | `efada8f2d64c` | `docs/ui0-spatial-minimal-audit` | `c05dac32ac9c` | draft |
| [#139](https://github.com/Masuku4129/kira_os_/pull/139) | feat(architecture): bounded PCI host bus enumeration | `architecture/pci-range-device-model-2026-11` | `ea83f83bdc59` | `architecture/target-baseline-2026-11` | `78a76003d0d8` | draft |
| [#138](https://github.com/Masuku4129/kira_os_/pull/138) | feat(gui): gated KIRA Spatial Minimal first layout slice | `docs/ui0-spatial-minimal-audit` | `c05dac32ac9c` | `main` | `811ca3a44ce0` | draft |
| [#137](https://github.com/Masuku4129/kira_os_/pull/137) | security: clear firmware PCI bus mastering before driver ownership | `security/pci-clear-firmware-busmaster` | `d35c00069bf4` | `main` | `bbc7f7ab948e` | ready |
| [#136](https://github.com/Masuku4129/kira_os_/pull/136) | security: block Ring 3 DMA/MSI drivers until IOMMU isolation | `security/kdrv-require-iommu-for-dma-msi` | `761a212f3ab7` | `main` | `bbc7f7ab948e` | ready |
| [#135](https://github.com/Masuku4129/kira_os_/pull/135) | Architecture: закрепить целевую архитектуру KIRA OS и план полного аудита | `architecture/target-baseline-2026-11` | `78a76003d0d8` | `main` | `bbc7f7ab948e` | draft |
| [#134](https://github.com/Masuku4129/kira_os_/pull/134) | feat(kvm): add KVP0 experimental interpreter prototype | `v46/kira-vm-core-v1` | `aa5378d2a1cc` | `v46/unified-release-integration` | `87f65b3f49cb` | draft |
| [#132](https://github.com/Masuku4129/kira_os_/pull/132) | fix(vmm): harden kernel stack slot lifecycle | `fix/vmm-kstack-double-free` | `2b8569ad1baf` | `v46/unified-release-integration` | `e9fb1151a50d` | draft |
| [#131](https://github.com/Masuku4129/kira_os_/pull/131) | fix(wasm): harden module validation and KIRA import ABI | `fix/wasm-module-header-bounds` | `55328f649baa` | `main` | `8d2904061423` | draft |
| [#125](https://github.com/Masuku4129/kira_os_/pull/125) | fix(iommu): combine hardened SMMUv3 domains and controller safety | `fix/arm-smmu-integrated-hardening` | `fee8c9aed19f` | `autodrv` | `e2c55cec08a9` | ready |
| [#121](https://github.com/Masuku4129/kira_os_/pull/121) | test(boot): align BIOS smoke test with image contents | `fix/boot-bios-smoke-no-initrd` | `029c18a47825` | `autodrv` | `6a66ebf6f45b` | ready |
| [#120](https://github.com/Masuku4129/kira_os_/pull/120) | fix(iommu): implement valid SMMUv3 stage-1 domains | `fix/arm-smmu-domain-encoding` | `932f39a8a305` | `autodrv` | `6a66ebf6f45b` | draft |
| [#116](https://github.com/Masuku4129/kira_os_/pull/116) | feat(fdt): resolve PCI requester IDs through iommu-map | `feat/fdt-iommu-map` | `6c37c9856998` | `autodrv` | `10a19683fa11` | draft |
| [#115](https://github.com/Masuku4129/kira_os_/pull/115) | fix(iommu): enforce SMMUv3 DMA address-width limits | `fix/arm-smmu-stage1-context-descriptor-v11` | `680d01376cf4` | `autodrv` | `10a19683fa11` | draft |
| [#112](https://github.com/Masuku4129/kira_os_/pull/112) | fix(boot): complete BIOS MBR and command-line handoff | `fix/bios-mbr-partition-cmdline-v2` | `2dcf255ff483` | `autodrv` | `10a19683fa11` | draft |
| [#95](https://github.com/Masuku4129/kira_os_/pull/95) | refactor(kcc): isolate remaining preprocessor and optimizer state | `ai/kcc-context-more-state` | `b7cb6f655128` | `ai/kcc-context-backend-regression` | `44396779377c` | draft |
| [#93](https://github.com/Masuku4129/kira_os_/pull/93) | test(kcc): verify sequential isolation across x86-64 and AArch64 | `ai/kcc-context-backend-regression` | `2f0e38254fbd` | `ai/kcc-context-x86-state` | `ace418857a25` | draft |
| [#81](https://github.com/Masuku4129/kira_os_/pull/81) | feat(mind): forecast memory pressure four seconds ahead | `ai/mind-memory-pressure-forecast-v2` | `579e807f74ef` | `ai/mind-validity-feature` | `7028f117ab6c` | draft |
| [#79](https://github.com/Masuku4129/kira_os_/pull/79) | perf(sched): replace timed-sleep scans with min-heap | `perf/scheduler-sleep-heap` | `5acdc5e359a4` | `autodrv` | `c75e01bb6144` | draft |
| [#78](https://github.com/Masuku4129/kira_os_/pull/78) | feat(mind): distinguish missing disk telemetry from zero latency | `ai/mind-validity-feature` | `464070a038c3` | `autodrv` | `10a19683fa11` | draft |
| [#76](https://github.com/Masuku4129/kira_os_/pull/76) | docs: audit system bottlenecks and define Mind vNext | `ai/mind-multitask-foundation` | `aeb9f2b7865c` | `autodrv` | `10a19683fa11` | draft |
| [#74](https://github.com/Masuku4129/kira_os_/pull/74) | perf(sched): reduce snapshot lock hold time | `perf/scheduler-snapshot-linear` | `5568c8f11dd8` | `autodrv` | `10a19683fa11` | draft |
| [#72](https://github.com/Masuku4129/kira_os_/pull/72) | fix(drivers): enforce exact per-file source licenses | `fix/bsd-driver-license-accuracy-rebased2` | `3b8f685caee5` | `autodrv` | `6a66ebf6f45b` | ready |
| [#63](https://github.com/Masuku4129/kira_os_/pull/63) | docs: record KIRA OS working contract and project constraints | `docs/user-work-contract-and-project-tracker` | `f6657bd55f20` | `autodrv` | `19b5a0662912` | draft |
| [#59](https://github.com/Masuku4129/kira_os_/pull/59) | CI: восемь параллельных боксов проверки KIRA OS | `factory/parallel-build-boxes-v3` | `556fc1837c95` | `autodrv` | `79d2d84e1d0f` | draft |
| [#58](https://github.com/Masuku4129/kira_os_/pull/58) | Architecture: decisions from November 2026 technology research | `work/architecture-selection-2026-11` | `024739047bd7` | `autodrv` | `79d2d84e1d0f` | draft |
| [#57](https://github.com/Masuku4129/kira_os_/pull/57) | Design: язык инструкций KIL для человека и ИИ | `work/kil-language-design-v2` | `9a9a92d1e507` | `autodrv` | `d7f0d08a5272` | draft |
| [#55](https://github.com/Masuku4129/kira_os_/pull/55) | docs: зафиксировать выбор технологий по результатам исследования | `work/architecture-decisions-2026-11` | `6b01625f2552` | `autodrv` | `d7f0d08a5272` | draft |
| [#54](https://github.com/Masuku4129/kira_os_/pull/54) | fix(sb16): diagnose and enforce ISA DMA ring constraints | `work/sb16-dma-diagnostics` | `49e83abc8809` | `autodrv` | `d7f0d08a5272` | draft |
| [#52](https://github.com/Masuku4129/kira_os_/pull/52) | CI: девять изолированных параллельных боксов разработки KIRA | `factory/parallel-build-boxes` | `ceccebf2b454` | `autodrv` | `234c83b0e513` | draft |
| [#51](https://github.com/Masuku4129/kira_os_/pull/51) | Design: KIL — декларативный язык инструкций для человека и ИИ | `work/kil-language-design` | `a1c8fdcc8af9` | `autodrv` | `1a388c2913e9` | draft |
| [#50](https://github.com/Masuku4129/kira_os_/pull/50) | Draft: единый syscall ABI x86-64 / ARM64 | `work/arch-parity-abi-v1` | `5ed9b1440523` | `autodrv` | `1c8980b0f6ff` | draft |
| [#49](https://github.com/Masuku4129/kira_os_/pull/49) | Isolation: private IPC objects and per-process capability boundary | `work/isolation-ipc-v1` | `9eae47fed27f` | `autodrv` | `b005226ad246` | draft |
| [#48](https://github.com/Masuku4129/kira_os_/pull/48) | Research: global KIRA validation program and full-system gates | `research/driver-adaptation-lab-2026-11` | `085325afcbe2` | `main` | `c63520432ea7` | draft |
| [#47](https://github.com/Masuku4129/kira_os_/pull/47) | Research: add KIRA data-format benchmark lab | `research/data-format-lab-2026-11` | `e7ca8f061de3` | `main` | `c63520432ea7` | draft |
| [#46](https://github.com/Masuku4129/kira_os_/pull/46) | Draft: KIRA OS unified release integration and UEFI boot validation | `v46/unified-release-integration` | `dacc04d0dafd` | `v46/kdrv-catalogue-signature-policy` | `796a5a0d66f4` | draft |
| [#45](https://github.com/Masuku4129/kira_os_/pull/45) | Harden x86-64 UEFI ELF loading and BootInfo conversion | `v46/uefi-bootinfo-validation` | `f7e05a2a20f2` | `v46/kdrv-catalogue-signature-policy` | `796a5a0d66f4` | draft |
| [#44](https://github.com/Masuku4129/kira_os_/pull/44) | Harden VT-d domains, SMMUv3 command queue and RTL8139 TX status | `v46/iommu-network-compile-fixes` | `b72a5b1f58bc` | `v46/kdrv-catalogue-signature-policy` | `796a5a0d66f4` | draft |
| [#43](https://github.com/Masuku4129/kira_os_/pull/43) | Prepare KIRA OS autonomous coding agent | `v46/kira-agent-bootstrap` | `89707a285bd7` | `v46/kdrv-catalogue-signature-policy` | `796a5a0d66f4` | draft |
| [#42](https://github.com/Masuku4129/kira_os_/pull/42) | KDRV: enforce publisher-required policy through install and kernel load | `v46/kdrv-catalogue-signature-policy` | `2d1ffeaa5fae` | `v46/kdrv-auth-integration-final` | `7504bda7cff2` | draft |
| [#41](https://github.com/Masuku4129/kira_os_/pull/41) | KDRV: authenticate publisher packages and make signed installs crash-safe | `v46/kdrv-auth-integration-final` | `5aefeb8cf1d0` | `v46/release-completion` | `bc98b6f07ca1` | ready |
| [#40](https://github.com/Masuku4129/kira_os_/pull/40) | KDRV: integrate publisher signatures and installer verification | `v46/kdrv-runtime-signature-import` | `7504bda7cff2` | `v46/release-completion` | `bc98b6f07ca1` | draft |
| [#39](https://github.com/Masuku4129/kira_os_/pull/39) | ci: exercise driver E2E for KDRV signature changes | `v46/kdrv-signature-ci-routing` | `de5928260f87` | `v46/ed25519-strict-subgroup` | `fb12562b5ca0` | draft |
| [#38](https://github.com/Masuku4129/kira_os_/pull/38) | crypto: reject Ed25519 small-order and identity public keys | `v46/ed25519-strict-subgroup` | `fb12562b5ca0` | `v46/kdrv-host-link-fix` | `39427ad9a8b5` | draft |
| [#36](https://github.com/Masuku4129/kira_os_/pull/36) | v46: fix KDRV signature host links and keyring identifiers | `v46/kdrv-keyring-array` | `03511e430e09` | `v46/kdrv-signature-work` | `d5b85234a518` | draft |
| [#35](https://github.com/Masuku4129/kira_os_/pull/35) | v46: integrate detached Ed25519 signatures for KDRV packages | `v46/kdrv-signature-work` | `d5b85234a518` | `v46/dma-device-cleanup` | `152a17a82c8a` | draft |
| [#32](https://github.com/Masuku4129/kira_os_/pull/32) | Add parallel architecture baseline factory | `factory/architecture-parallel` | `4afb6a78f9ec` | `work/ci-drvman-log-fix` | `115827363081` | draft |
| [#24](https://github.com/Masuku4129/kira_os_/pull/24) | Fix Driver Manager CI diagnostics and package listing | `work/ci-drvman-log-fix` | `115827363081` | `work/kdrv-auth` | `31d186176f69` | draft |
| [#23](https://github.com/Masuku4129/kira_os_/pull/23) | ci: add KIRA OS parallel build factory notifications | `work/parallel-build-factory` | `96a115425854` | `v46/architecture-completion` | `813b1de9cf0a` | draft |
| [#22](https://github.com/Masuku4129/kira_os_/pull/22) | Harden Ed25519 verification against small-order points | `work/ed25519-subgroup-validation` | `0a3744829b53` | `v46/architecture-completion` | `813b1de9cf0a` | draft |
| [#21](https://github.com/Masuku4129/kira_os_/pull/21) | KIRA OS v46: harden release gate and authenticate KDRV packages | `work/kdrv-auth` | `31d186176f69` | `v46/architecture-completion` | `03735915ada0` | draft |
| [#12](https://github.com/Masuku4129/kira_os_/pull/12) | [Draft] v46 release completion: audit fixes and architecture blockers | `v46/release-completion` | `c33423216670` | `v45/overhaul` | `c07a7a3465fa` | draft |

## Правила Stage 0 для PR-графа

- [ ] Для каждого PR проверить diff, ancestry, changed files, CI на exact head SHA, review/comments и прямые/транзитивные зависимости.
- [ ] Отдельно классифицировать: актуальный кандидат, stacked child, устаревший, дублирующий, superseded, конфликтующий или требующий rebase.
- [ ] Для PR с одинаковым base SHA объединять только после проверки семантических конфликтов и сохранения всех изменений.
- [ ] Для PR с base branch не равным `main` не менять target автоматически; сначала установить, является ли ветка частью обязательной цепочки.
- [ ] Проверить, что закрытие/слияние старых PR не удалит единственную копию полезного изменения.
- [ ] Сверить каждый заявленный PASS с конкретным workflow run и SHA.
- [ ] Сохранить исходные ветки и артефакты до завершения инвентаризации; не force-push и не переписывать `main`.

## Ограничение

Этот реестр фиксирует метаданные PR (head/base refs и SHA). Он не является code review всех 65 diff и не доказывает, что PR совместимы, безопасны или готовы к merge. Source-level review и exact-head workflow audit остаются частью Stage 0.


## 2026-11-15 refresh — audit PR #151 ancestry and current-main drift

- Current `main` remains `3497f48fe769dc61d90ee63da8cc7fc85d6eab7e`.
- Audit PR #151 current head: `0c28f4f689e15251822b80eb07f91da6c1d78bc7`; base remains pinned `811ca3a44ce0d3d59812b28a7ea0cfacdad2d5b4`.
- GitHub compare of current main to audit head: `diverged`, audit branch **422 commits ahead / 6 behind**. GitHub reports `mergeable=false`.
- The large ahead count is ancestry/history, not 422 changed files. The compare reports 22 changed paths, all documentation: audit/handoff/architecture/stage tracker/worklog/ADR/matrix files. No production source path is in this audit PR diff.
- The six commits now on main after the pinned base are:
  1. `8a9fd97` — TCP lifecycle candidate CI record;
  2. `1944bc6` — KCC single-active compilation contract;
  3. `b8560dc` — TCP socket state/wakeup synchronization;
  4. `d2557ad` — revert restoring `net.c` after unintended default-branch write;
  5. `12cf3fb` — WASM import validation and signed LEB change (introduces the F-062 current-main compile failure);
  6. `3497f48` — gfx event queue serialization.
- Decision: do not merge PR #151 or rewrite its history during Stage 0. It is a documentation-only PR, but its base/ancestry must be reconciled explicitly before any integration. Main remains untouched.
